#include <Backend/EditorSession.hpp>
#include <Backend/PlayerController.hpp>
#include <Models/ProjectCodec.hpp>

#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <cmath>
#include <functional>
#include <limits>

namespace {
using heisenberg::EditStatus;
using heisenberg::ui::EditorSession;
using heisenberg::ui::PlayerController;

bool waitForSource(const std::function<bool()>& condition) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 10000) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    return condition();
}

class SourceRangeTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(directory.isValid());
        ASSERT_TRUE(session.importFiles({HEISENBERG_TEST_MEDIA}).isEmpty());
        ref = QString::fromStdString(session.editor().manifest().entries().front().id);
        ASSERT_DOUBLE_EQ(*session.editor().manifest().entries().front().sourceFPS, 24.0);
        ASSERT_DOUBLE_EQ(session.editor().activeTimeline()->fps, 30.0);
        package = directory.path() + "/source.berg";
        ASSERT_TRUE(session.savePackage(package));
    }

    std::string json() const {
        return heisenberg::ProjectCodec::encode(session.editor().project());
    }

    EditorSession session;
    QTemporaryDir directory;
    QString ref, package;
};

TEST_F(SourceRangeTest, UnmarkedPlacementRetainsFullSourceDefault) {
    qint64 trim = -1, duration = 0;
    ASSERT_TRUE(session.sourcePlacementRange(ref, trim, duration));
    EXPECT_EQ(trim, 0);
    EXPECT_EQ(duration, 30);
    ASSERT_TRUE(session.placeMedia(ref, 10).changed());
    const auto& clip = session.editor().activeTrack()->clips.front();
    EXPECT_EQ(clip.startFrame, 10);
    EXPECT_EQ(clip.trimStartFrame, 0);
    EXPECT_EQ(clip.durationFrames, 30);
    EXPECT_EQ(clip.trimEndFrame, 0);
}

TEST_F(SourceRangeTest, SeparatelyRoundedBoundariesUseTimelineNotSourceFps) {
    // round((Out-In)*30)=6, whereas round(Out*30)-round(In*30)=5.
    ASSERT_TRUE(session.setSourceIn(ref, 0.25));
    ASSERT_TRUE(session.setSourceOut(ref, 0.44));
    qint64 trim = -1, duration = -1;
    ASSERT_TRUE(session.sourcePlacementRange(ref, trim, duration));
    EXPECT_EQ(trim, 8);
    EXPECT_EQ(duration, 5);
    ASSERT_TRUE(session.placeMedia(ref, 12).changed());
    const auto& clip = session.editor().activeTrack()->clips.front();
    EXPECT_EQ(clip.trimStartFrame, 8);
    EXPECT_EQ(clip.durationFrames, 5);
    EXPECT_EQ(clip.trimEndFrame, 17);
}

TEST_F(SourceRangeTest, OneEndedRangesDefaultToSourceBoundaries) {
    qint64 trim = -1, duration = -1;
    ASSERT_TRUE(session.setSourceIn(ref, 0.25));
    EXPECT_FALSE(session.sourceRange(ref).outSeconds.has_value());
    ASSERT_TRUE(session.sourcePlacementRange(ref, trim, duration));
    EXPECT_EQ(trim, 8);
    EXPECT_EQ(duration, 22);
    ASSERT_TRUE(session.clearSourceRange(ref));
    ASSERT_TRUE(session.setSourceOut(ref, 0.44));
    EXPECT_FALSE(session.sourceRange(ref).inSeconds.has_value());
    ASSERT_TRUE(session.sourcePlacementRange(ref, trim, duration));
    EXPECT_EQ(trim, 0);
    EXPECT_EQ(duration, 13);
}

TEST_F(SourceRangeTest, MarksRemainPerAssetAndResolveAgainWhenTimelineFpsChanges) {
    ASSERT_TRUE(session.importFiles({HEISENBERG_TEST_MP3}).isEmpty());
    const auto audioRef = QString::fromStdString(session.editor().manifest().entries().back().id);
    ASSERT_TRUE(session.setSourceIn(ref, 0.25));
    ASSERT_TRUE(session.setSourceOut(ref, 0.44));
    ASSERT_TRUE(session.setSourceOut(audioRef, 0.5));
    EXPECT_FALSE(session.sourceRange(audioRef).inSeconds.has_value());
    EXPECT_DOUBLE_EQ(*session.sourceRange(audioRef).outSeconds, 0.5);
    EXPECT_DOUBLE_EQ(*session.sourceRange(ref).inSeconds, 0.25);
    ASSERT_TRUE(session.applyTimelineSettings(
        heisenberg::TimelineSettings::fromTimelineSettings(320, 240, 60)).changed());
    qint64 trim = -1, duration = -1;
    ASSERT_TRUE(session.sourcePlacementRange(ref, trim, duration));
    EXPECT_EQ(trim, 15);
    EXPECT_EQ(duration, 11);
    ASSERT_TRUE(session.clearSourceRange(audioRef));
    EXPECT_TRUE(session.sourceRange(ref).inSeconds.has_value());
}

TEST_F(SourceRangeTest, InvalidMarksAreAtomicAndRoundedZeroPlacementIsRejected) {
    int notifications = 0;
    QObject::connect(&session, &EditorSession::sourceRangeChanged, [&](const QString&) { ++notifications; });
    const double sourceDuration = session.editor().manifest().entries().front().duration;
    for (double invalid : {-0.1, sourceDuration + 0.1,
                           std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()}) {
        EXPECT_FALSE(session.setSourceIn(ref, invalid));
        EXPECT_FALSE(session.setSourceOut(ref, invalid));
    }
    EXPECT_FALSE(session.setSourceIn(ref, sourceDuration));
    EXPECT_FALSE(session.setSourceOut(ref, 0.0));
    EXPECT_FALSE(session.setSourceIn("unknown", 0.1));
    EXPECT_FALSE(session.clearSourceRange("unknown"));
    EXPECT_FALSE(session.sourceRange("unknown").inSeconds.has_value());
    EXPECT_EQ(notifications, 0);
    ASSERT_TRUE(session.setSourceIn(ref, 0.25));
    ASSERT_TRUE(session.setSourceOut(ref, 0.44));
    const auto valid = session.sourceRange(ref);
    EXPECT_FALSE(session.setSourceIn(ref, 0.44));
    EXPECT_FALSE(session.setSourceOut(ref, 0.25));
    EXPECT_EQ(session.sourceRange(ref), valid);
    EXPECT_EQ(notifications, 2);
    ASSERT_TRUE(session.clearSourceRange(ref));
    ASSERT_TRUE(session.setSourceIn(ref, 0.001));
    ASSERT_TRUE(session.setSourceOut(ref, 0.002));
    qint64 trim = 123, duration = 456;
    QString error;
    EXPECT_FALSE(session.sourcePlacementRange(ref, trim, duration, &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(trim, 123);
    EXPECT_EQ(duration, 456);
    const auto before = json();
    EXPECT_EQ(session.placeMedia(ref, 0).status, EditStatus::Invalid);
    EXPECT_EQ(session.insertMedia(ref, 0).status, EditStatus::Invalid);
    EXPECT_EQ(json(), before);
    EXPECT_FALSE(session.isDirty());
}

TEST_F(SourceRangeTest, MarksHaveNoDocumentDirtyHistoryOrTimelineEffects) {
    ASSERT_TRUE(session.placeMedia(ref, 0).changed());
    const auto placed = json();
    session.undo();
    ASSERT_TRUE(session.savePackage(package));
    const auto before = json();
    const auto manifest = session.editor().manifest();
    const auto revision = session.editor().revision();
    int timelineChanges = 0, dirtyChanges = 0, notifications = 0;
    QObject::connect(&session, &EditorSession::timelineChanged, [&](qint64) { ++timelineChanges; });
    QObject::connect(&session, &EditorSession::dirtyChanged, [&](bool) { ++dirtyChanges; });
    QObject::connect(&session, &EditorSession::sourceRangeChanged, [&](const QString& changedRef) {
        EXPECT_EQ(changedRef, ref);
        ++notifications;
    });
    ASSERT_TRUE(session.setSourceIn(ref, 0.25));
    ASSERT_TRUE(session.setSourceIn(ref, 0.25)); // Accepted no-op emits nothing.
    ASSERT_TRUE(session.setSourceOut(ref, 0.75));
    ASSERT_TRUE(session.clearSourceRange(ref));
    ASSERT_TRUE(session.clearSourceRange(ref));
    EXPECT_EQ(notifications, 3);
    EXPECT_EQ(timelineChanges, 0);
    EXPECT_EQ(dirtyChanges, 0);
    EXPECT_FALSE(session.isDirty());
    EXPECT_EQ(session.editor().revision(), revision);
    EXPECT_EQ(session.editor().manifest(), manifest);
    EXPECT_EQ(json(), before);
    session.redo();
    EXPECT_EQ(json(), placed);
}

TEST_F(SourceRangeTest, SuccessfulLoadClearsMarksAndFailedLoadPreservesThem) {
    ASSERT_TRUE(session.setSourceIn(ref, 0.25));
    ASSERT_TRUE(session.setSourceOut(ref, 0.75));
    const auto marks = session.sourceRange(ref);
    const auto token = session.token();
    EXPECT_FALSE(session.loadPackage(directory.path() + "/missing.berg"));
    EXPECT_EQ(session.sourceRange(ref), marks);
    EXPECT_EQ(session.token(), token);
    ASSERT_TRUE(session.savePackage(package));
    EditorSession other;
    ASSERT_TRUE(other.loadPackage(package));
    EXPECT_FALSE(other.sourceRange(ref).inSeconds.has_value());
    int cleared = 0;
    QObject::connect(&session, &EditorSession::sourceRangeChanged, [&](const QString& changedRef) {
        if (changedRef == ref) {
            EXPECT_FALSE(session.sourceRange(ref).inSeconds.has_value());
            EXPECT_FALSE(session.sourceRange(ref).outSeconds.has_value());
            ++cleared;
        }
    });
    ASSERT_TRUE(session.loadPackage(package));
    EXPECT_EQ(cleared, 1);
    EXPECT_NE(session.token(), token);
    EXPECT_FALSE(session.isDirty());
}

TEST_F(SourceRangeTest, InsertAndRippleDeleteUseMarkedDurationAndSingleUndoSteps) {
    ASSERT_TRUE(session.placeMedia(ref, 0).changed());
    ASSERT_TRUE(session.placeMedia(ref, 60).changed());
    ASSERT_TRUE(session.setSourceIn(ref, 0.25));
    ASSERT_TRUE(session.setSourceOut(ref, 0.75));
    const auto before = json();
    const auto inserted = session.insertMedia(ref, 30);
    ASSERT_TRUE(inserted.changed()) << inserted.message;
    const auto* track = session.editor().activeTrack();
    ASSERT_EQ(track->clips.size(), 3u);
    const auto insertedId = QString::fromStdString(track->clips[1].id);
    EXPECT_EQ(track->clips[1].startFrame, 30);
    EXPECT_EQ(track->clips[1].trimStartFrame, 8);
    EXPECT_EQ(track->clips[1].durationFrames, 15);
    EXPECT_EQ(track->clips[2].startFrame, 75);
    const auto afterInsert = json();
    session.undo();
    EXPECT_EQ(json(), before);
    session.redo();
    EXPECT_EQ(json(), afterInsert);
    ASSERT_TRUE(session.rippleDeleteClip(insertedId).changed());
    const auto afterDelete = json();
    EXPECT_EQ(session.editor().activeTrack()->clips.size(), 2u);
    EXPECT_EQ(session.editor().activeTrack()->clips.back().startFrame, 60);
    session.undo();
    EXPECT_EQ(json(), afterInsert);
    session.redo();
    EXPECT_EQ(json(), afterDelete);
}

TEST_F(SourceRangeTest, ControllerGuardsMarksUntilReadyAndUsesActualSourceFpsForInclusiveOut) {
    bool identityAtSignal = false;
    int notifications = 0;
    PlayerController player(&session);
    player.setHardwareDecode(false);
    EXPECT_FALSE(player.sourceMarkingAvailable());
    EXPECT_TRUE(player.currentSourceMediaRef().isEmpty());
    player.markSourceIn();
    player.markSourceOut();
    EXPECT_FALSE(session.sourceRange(ref).inSeconds.has_value());
    QObject::connect(&player, &PlayerController::previewModeChanged, [&](bool source, const QString&) {
        if (source) {
            identityAtSignal = player.currentSourceMediaRef() == ref;
            EXPECT_FALSE(player.sourceMarkingAvailable());
        }
    });
    player.previewAsset(ref);
    EXPECT_TRUE(identityAtSignal);
    EXPECT_FALSE(player.sourceMarkingAvailable());
    player.markSourceOut();
    EXPECT_FALSE(session.sourceRange(ref).outSeconds.has_value());
    ASSERT_TRUE(waitForSource([&]() { return player.sourceMarkingAvailable(); }));
    QObject::connect(&player, &PlayerController::sourceMarkingChanged, [&]() { ++notifications; });
    player.markSourceIn();
    player.markSourceOut();
    ASSERT_TRUE(session.sourceRange(ref).inSeconds.has_value());
    ASSERT_TRUE(session.sourceRange(ref).outSeconds.has_value());
    EXPECT_DOUBLE_EQ(*session.sourceRange(ref).inSeconds, 0.0);
    EXPECT_NEAR(*session.sourceRange(ref).outSeconds, 1.0 / 24.0, 1e-9);
    EXPECT_GE(notifications, 2);
    player.clearSourceRange();
    EXPECT_FALSE(session.sourceRange(ref).inSeconds.has_value());
    player.goToEnd();
    ASSERT_TRUE(waitForSource([&]() { return player.sourceMarkingAvailable() && player.currentTime() > 0.9; }));
    player.markSourceOut();
    ASSERT_TRUE(session.sourceRange(ref).outSeconds.has_value());
    EXPECT_LE(*session.sourceRange(ref).outSeconds, session.editor().manifest().entries().front().duration);
    EXPECT_NEAR(*session.sourceRange(ref).outSeconds, 1.0, 1e-9);
    EXPECT_FALSE(session.isDirty());
    player.closeFile();
    EXPECT_TRUE(player.currentSourceMediaRef().isEmpty());
    EXPECT_FALSE(player.sourceMarkingAvailable());
    const auto marks = session.sourceRange(ref);
    player.clearSourceRange();
    player.markSourceIn();
    EXPECT_EQ(session.sourceRange(ref), marks);
}

TEST_F(SourceRangeTest, ControllerClearsActiveIdentityOnSwitchLoadCloseAndFailure) {
    PlayerController player(&session);
    player.setHardwareDecode(false);
    player.previewAsset(ref);
    ASSERT_TRUE(waitForSource([&]() { return player.sourceMarkingAvailable(); }));
    player.markSourceOut();
    ASSERT_TRUE(session.loadPackage(package));
    EXPECT_FALSE(player.sourceMarkingAvailable());
    EXPECT_TRUE(player.currentSourceMediaRef().isEmpty());
    EXPECT_FALSE(session.sourceRange(ref).outSeconds.has_value());
    player.previewAsset(ref);
    ASSERT_TRUE(waitForSource([&]() { return player.sourceMarkingAvailable(); }));
    player.previewAsset("missing-ref");
    EXPECT_FALSE(player.sourceMarkingAvailable());
    EXPECT_TRUE(player.currentSourceMediaRef().isEmpty());
    player.previewAsset(ref);
    ASSERT_TRUE(waitForSource([&]() { return player.sourceMarkingAvailable(); }));
    // Opening a path directly has no manifest identity, even when its media is valid.
    ASSERT_TRUE(player.openFile(HEISENBERG_TEST_MEDIA));
    EXPECT_TRUE(player.currentSourceMediaRef().isEmpty());
    EXPECT_FALSE(player.sourceMarkingAvailable());
    player.closeFile();
    const QString broken = directory.path() + "/broken.mp4";
    QFile file(broken);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_GT(file.write("invalid media"), 0);
    file.close();
    bool failed = false;
    QObject::connect(&player, &PlayerController::playbackFailed, [&](const QString&) { failed = true; });
    ASSERT_TRUE(player.openFile(broken));
    ASSERT_TRUE(waitForSource([&]() { return failed; }));
    EXPECT_TRUE(player.currentSourceMediaRef().isEmpty());
    EXPECT_FALSE(player.sourceMarkingAvailable());
    EXPECT_TRUE(player.currentFile().isEmpty());
}
} // namespace
