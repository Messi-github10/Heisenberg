#include <Backend/EditorSession.hpp>
#include <MainWidget/TimelineWidget.hpp>
#include <Models/ClipAppearance.hpp>
#include <Models/ProjectCodec.hpp>

#include <gtest/gtest.h>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QImage>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QWheelEvent>
#include <algorithm>
#include <limits>
#include <vector>

namespace {
using namespace heisenberg;
using heisenberg::ui::EditorSession;

KeyframeTrackPayload markerTrack(std::initializer_list<int64_t> frames, JsonValue value) {
    auto keys = JsonValue::array();
    for (const auto frame : frames)
        keys.push_back({{"frame", frame}, {"value", value}, {"interpolationOut", "linear"}});
    return {JsonValue{{"keyframes", keys}}};
}

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

bool gold(QColor color) {
    return color.red() > 200 && color.green() > 150 && color.green() < 225 && color.blue() < 135;
}

int goldPixels(const QImage& image, QRect area) {
    area = area.intersected(image.rect());
    int count = 0;
    for (int y = area.top(); y <= area.bottom(); ++y)
        for (int x = area.left(); x <= area.right(); ++x)
            if (gold(image.pixelColor(x, y))) ++count;
    return count;
}

std::vector<int> markerCenters(const QImage& image, int y) {
    std::vector<int> centers;
    int first = -1;
    for (int x = 160; x < image.width(); ++x) {
        const bool hit = goldPixels(image, QRect(x, y - 2, 1, 4)) > 0;
        if (hit && first < 0) first = x;
        if (!hit && first >= 0) {
            centers.push_back((first + x - 1) / 2);
            first = -1;
        }
    }
    if (first >= 0) centers.push_back((first + image.width() - 1) / 2);
    return centers;
}

class AppearanceMarkerUiTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(directory.isValid());
        package = directory.filePath("markers.berg");
        ASSERT_TRUE(session.savePackage(package));
        project = session.editor().project();
        Timeline active;
        active.id = "markers";
        Track video;
        video.id = "video";
        Clip first;
        first.id = "first";
        first.mediaRef = "video";
        first.startFrame = 20;
        first.durationFrames = 80;
        // Local markers must not depend on source trim or playback speed.
        first.trimStartFrame = 13;
        first.speed = 2.0;
        first.opacityTrack = markerTrack({-10, 0, 10, 79, 80, 100}, 0.75);
        first.positionTrack = markerTrack({10, 20}, JsonValue{{"a", 0.5}, {"b", 0.5}});
        first.scaleTrack = markerTrack({20, 30}, JsonValue{{"a", 1.0}, {"b", 1.0}});
        first.rotationTrack = markerTrack({30, 40}, 0.0);
        first.cropTrack = markerTrack({40}, JsonValue{{"left", 0.0}, {"top", 0.0},
                                                                   {"right", 0.0}, {"bottom", 0.0}});
        video.clips = {first};
        Track second = video;
        second.id = "second-video";
        second.displayHeight = 32;
        second.clips[0].id = "second";
        Track audio;
        audio.id = "audio-track";
        audio.type = ClipType::Audio;
        auto audioClip = first;
        audioClip.id = "audio";
        audioClip.mediaRef = "audio";
        audioClip.mediaType = audioClip.sourceClipType = ClipType::Audio;
        audio.clips = {audioClip};
        active.tracks = {video, second, audio};
        project.timelines = {active};
        project.activeTimelineId = active.id;
        project.openTimelineIds = std::vector<std::string>{active.id};
        project.viewStates.reset();
        auto manifest = session.editor().manifest();
        for (const auto type : {ClipType::Video, ClipType::Audio}) {
            MediaManifestEntry entry;
            entry.id = type == ClipType::Video ? "video" : "audio";
            entry.type = type;
            entry.duration = 30.0;
            entry.source.path = directory.path().toStdString() + "/offline-" + entry.id;
            ASSERT_TRUE(manifest.add(entry));
        }
        ASSERT_NO_FATAL_FAILURE(writeDocument("media.json", manifest.toJson()));
        ASSERT_NO_FATAL_FAILURE(reload());
        widget.setSession(&session);
        widget.resize(800, 240);
        widget.show();
        QCoreApplication::processEvents();
    }

    void writeDocument(const QString& name, const std::string& text) {
        QFile file(package + '/' + name);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        ASSERT_EQ(file.write(text.data(), static_cast<qint64>(text.size())), static_cast<qint64>(text.size()));
    }
    void reload() {
        ASSERT_NO_FATAL_FAILURE(writeDocument("project.json", ProjectCodec::encode(project)));
        QString error;
        ASSERT_TRUE(session.loadPackage(package, &error)) << error.toStdString();
    }
    QImage paint() {
        QImage image(widget.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        widget.render(&image);
        return image;
    }
    void selectFirst() {
        click(widget, QPointF(280, 68));
        ASSERT_EQ(widget.selectedClipId(), "first");
    }
    void wheel(QPoint angle, Qt::KeyboardModifiers modifiers, QPointF anchor = QPointF(160, 15)) {
        QWheelEvent event(anchor, widget.mapToGlobal(anchor.toPoint()), {}, angle,
                          Qt::NoButton, modifiers, Qt::NoScrollPhase, false);
        QApplication::sendEvent(&widget, &event);
    }
    Clip& first() { return project.timelines[0].tracks[0].clips[0]; }
    std::string json() const { return ProjectCodec::encode(session.editor().project()); }

    QTemporaryDir directory;
    EditorSession session;
    TimelineWidget widget;
    ProjectFile project;
    QString package;
};

TEST_F(AppearanceMarkerUiTest, PaintsUnionOfFivePropertiesOnlyOnSelectedVideoClips) {
    const auto before = json();
    EXPECT_TRUE(markerCenters(paint(), 86).empty());
    ASSERT_NO_FATAL_FAILURE(selectFirst());
    auto image = paint();
    const std::vector<int> expected{240, 280, 320, 360, 400, 556};
    const auto centers = markerCenters(image, 86);
    ASSERT_EQ(centers.size(), expected.size());
    for (size_t i = 0; i < centers.size(); ++i) EXPECT_NEAR(centers[i], expected[i], 1);
    EXPECT_TRUE(markerCenters(image, 118).empty());
    EXPECT_EQ(goldPixels(image, QRect(240, 53, 320, 28)), 0); // Label and detail rows.
    EXPECT_EQ(goldPixels(image, QRect(160, 80, 77, 12)), 0); // Negative local key.
    EXPECT_EQ(goldPixels(image, QRect(560, 80, 120, 12)), 0); // Duration is exclusive.
    click(widget, QPointF(280, 144), Qt::ControlModifier);
    ASSERT_TRUE(widget.selectedClipIds().contains("audio"));
    image = paint();
    EXPECT_EQ(goldPixels(image, QRect(160, 130, 640, 42)), 0);
    EXPECT_EQ(json(), before);
    EXPECT_FALSE(session.isDirty());
}

TEST_F(AppearanceMarkerUiTest, SecondaryAndMinimumHeightSelectionKeepMarkersBelowText) {
    ASSERT_NO_FATAL_FAILURE(selectFirst());
    click(widget, QPointF(280, 106), Qt::ControlModifier);
    ASSERT_EQ(widget.selectedClipId(), "second");
    ASSERT_EQ(widget.selectedClipIds().size(), 2);
    const auto image = paint();
    EXPECT_EQ(markerCenters(image, 86).size(), 6u);
    EXPECT_EQ(markerCenters(image, 118).size(), 6u);
    EXPECT_EQ(goldPixels(image, QRect(240, 97, 320, 16)), 0);
    EXPECT_EQ(goldPixels(image, QRect(160, 124, 640, 3)), 0); // Outside clip body.
}

TEST_F(AppearanceMarkerUiTest, LiveMarqueePaintsEveryHitBeforeSelectionCommits) {
    mouse(widget, QEvent::MouseButtonPress, QPointF(220, 51));
    mouse(widget, QEvent::MouseMove, QPointF(600, 125));
    EXPECT_TRUE(widget.selectedClipIds().isEmpty());
    const auto preview = paint();
    EXPECT_GT(goldPixels(preview, QRect(275, 82, 10, 8)), 0);
    EXPECT_GT(goldPixels(preview, QRect(275, 114, 10, 8)), 0);
    mouse(widget, QEvent::MouseButtonRelease, QPointF(600, 125));
    EXPECT_EQ(widget.selectedClipIds().size(), 2);
    EXPECT_EQ(markerCenters(paint(), 86).size(), 6u);
    EXPECT_EQ(markerCenters(paint(), 118).size(), 6u);
}

TEST_F(AppearanceMarkerUiTest, ExactFrameDuplicatesProduceTheSamePaintAsOneTrack) {
    first().opacityTrack = markerTrack({10, 20}, 0.75);
    first().positionTrack.reset();
    first().scaleTrack.reset();
    first().rotationTrack.reset();
    first().cropTrack.reset();
    ASSERT_NO_FATAL_FAILURE(reload());
    ASSERT_NO_FATAL_FAILURE(selectFirst());
    const auto single = paint();
    first().positionTrack = markerTrack({10, 20}, JsonValue{{"a", 0.5}, {"b", 0.5}});
    first().scaleTrack = markerTrack({10, 20}, JsonValue{{"a", 1.0}, {"b", 1.0}});
    first().rotationTrack = markerTrack({10, 20}, 0.0);
    first().cropTrack = markerTrack({10, 20}, JsonValue{{"left", 0.0}, {"top", 0.0},
                                                                {"right", 0.0}, {"bottom", 0.0}});
    ASSERT_NO_FATAL_FAILURE(reload());
    ASSERT_NO_FATAL_FAILURE(selectFirst());
    EXPECT_EQ(paint(), single);
}

TEST_F(AppearanceMarkerUiTest, MalformedPropertiesAreSkippedWithoutHidingValidNeighborsOrMutatingPayloads) {
    first().opacityTrack = markerTrack({12}, 0.75);
    first().positionTrack = KeyframeTrackPayload{JsonValue{{"opaque", true}}};
    first().scaleTrack = markerTrack({20}, JsonValue{{"a", -1.0}, {"b", 1.0}});
    first().rotationTrack = markerTrack({30}, 0.0);
    first().rotationTrack->value["keyframes"][0]["frame"] = 30.5;
    first().cropTrack = markerTrack({40}, JsonValue{{"left", 0.8}, {"top", 0.0},
                                                                {"right", 0.8}, {"bottom", 0.0}});
    ASSERT_NO_FATAL_FAILURE(reload());
    const auto before = json();
    ASSERT_NO_FATAL_FAILURE(selectFirst());
    const auto centers = markerCenters(paint(), 86);
    ASSERT_EQ(centers.size(), 1u);
    EXPECT_NEAR(centers[0], 288, 1);
    EXPECT_EQ(json(), before);
    EXPECT_FALSE(session.isDirty());
}

TEST_F(AppearanceMarkerUiTest, OutOfRangeAndExtremeKeysDoNotPaintAtEitherClipEdge) {
    first().opacityTrack = markerTrack({std::numeric_limits<int64_t>::min(), -1, 80,
                                        std::numeric_limits<int64_t>::max()}, 0.75);
    first().positionTrack.reset();
    first().scaleTrack.reset();
    first().rotationTrack.reset();
    first().cropTrack.reset();
    ASSERT_NO_FATAL_FAILURE(reload());
    ASSERT_NO_FATAL_FAILURE(selectFirst());
    EXPECT_TRUE(markerCenters(paint(), 86).empty());
}

TEST_F(AppearanceMarkerUiTest, ClickingAMarkerRetainsOrdinaryClipSelectionBehavior) {
    const auto before = json();
    int scrubs = 0, edits = 0;
    QObject::connect(&widget, &TimelineWidget::scrubStarted, &widget, [&] { ++scrubs; });
    QObject::connect(&widget, &TimelineWidget::editRequested, &widget, [&] { ++edits; });
    ASSERT_NO_FATAL_FAILURE(selectFirst());
    click(widget, QPointF(280, 86));
    EXPECT_EQ(widget.selectedClipId(), "first");
    EXPECT_EQ(widget.currentFrame(), 0);
    EXPECT_EQ(scrubs, 0);
    EXPECT_EQ(edits, 0);
    EXPECT_EQ(json(), before);
}

TEST_F(AppearanceMarkerUiTest, ZoomAndHorizontalScrollUseTheClipCoordinateSystemAndClipAtViewport) {
    const auto before = json();
    ASSERT_NO_FATAL_FAILURE(selectFirst());
    wheel(QPoint(0, 120), Qt::ControlModifier); // 5px/frame, anchored at timeline origin.
    auto centers = markerCenters(paint(), 86);
    const std::vector<int> zoomed{260, 310, 360, 410, 460, 655};
    ASSERT_EQ(centers.size(), zoomed.size());
    for (size_t i = 0; i < centers.size(); ++i) EXPECT_NEAR(centers[i], zoomed[i], 1);
    // Narrow the viewport so scrolling is possible, then scroll 160px right.
    widget.resize(480, 240);
    wheel(QPoint(0, -240), Qt::ShiftModifier);
    auto image = paint();
    EXPECT_EQ(goldPixels(image, QRect(0, 80, 160, 12)), 0);
    EXPECT_GT(goldPixels(image, QRect(197, 82, 7, 8)), 0); // local20: 360 - 160.
    EXPECT_GT(goldPixels(image, QRect(247, 82, 7, 8)), 0);
    EXPECT_GT(goldPixels(image, QRect(297, 82, 7, 8)), 0);
    EXPECT_EQ(json(), before);
}

TEST_F(AppearanceMarkerUiTest, VerticalScrollMovesMarkersWithTheirClipBody) {
    ASSERT_NO_FATAL_FAILURE(selectFirst());
    click(widget, QPointF(280, 106), Qt::ControlModifier);
    widget.resize(800, 142);
    auto* bar = widget.findChild<QScrollBar*>("timelineVerticalScrollBar");
    ASSERT_NE(bar, nullptr);
    ASSERT_GE(bar->maximum(), 12);
    bar->setValue(12);
    const auto image = paint();
    EXPECT_EQ(markerCenters(image, 74).size(), 6u);
    EXPECT_EQ(markerCenters(image, 106).size(), 6u);
    EXPECT_EQ(goldPixels(image, QRect(160, 114, 640, 4)), 0);
}

TEST_F(AppearanceMarkerUiTest, RefreshReplacesCachedMarkersAfterAppearanceEditAndUndo) {
    first().opacityTrack = markerTrack({10}, 0.75);
    first().positionTrack.reset();
    first().scaleTrack.reset();
    first().rotationTrack.reset();
    first().cropTrack.reset();
    ASSERT_NO_FATAL_FAILURE(reload());
    ASSERT_NO_FATAL_FAILURE(selectFirst());
    ASSERT_EQ(markerCenters(paint(), 86).size(), 1u);
    ClipAppearanceEdit edit;
    edit.property = AppearanceProperty::Opacity;
    edit.localFrame = 10;
    edit.action = AppearanceEditAction::ToggleKeyframe;
    ASSERT_TRUE(session.editClipAppearance("first", edit).changed());
    EXPECT_TRUE(markerCenters(paint(), 86).empty());
    session.undo();
    EXPECT_EQ(markerCenters(paint(), 86).size(), 1u);
}

TEST(TimelineCurrentFrameSignalTest, PlaybackAndFrameCountClampEmitOnlyForStoredChanges) {
    TimelineWidget widget;
    std::vector<qint64> frames;
    QObject::connect(&widget, &TimelineWidget::currentFrameChanged, &widget,
                     [&](qint64 frame) { EXPECT_EQ(widget.currentFrame(), frame); frames.push_back(frame); });
    widget.setFrameCount(100);
    widget.setPlaying(true);
    widget.setCurrentFrame(10);
    widget.setCurrentFrame(10);
    widget.setCurrentFrame(200);
    widget.setCurrentFrame(101);
    widget.setFrameCount(20);
    widget.setFrameCount(20);
    widget.setFrameCount(-1);
    widget.setCurrentFrame(-10);
    EXPECT_EQ(frames, (std::vector<qint64>{10, 99, 19, 0}));
}

TEST_F(AppearanceMarkerUiTest, ScrubAndEmptySpaceEmitChangeOnlyWhileKeepingTransportRequests) {
    std::vector<qint64> frames;
    int requests = 0, finishes = 0;
    QObject::connect(&widget, &TimelineWidget::currentFrameChanged, &widget,
                     [&](qint64 frame) { frames.push_back(frame); });
    QObject::connect(&widget, &TimelineWidget::scrubFrameRequested, &widget, [&](qint64) { ++requests; });
    QObject::connect(&widget, &TimelineWidget::scrubFinished, &widget, [&](qint64) { ++finishes; });
    mouse(widget, QEvent::MouseButtonPress, QPointF(200, 15));
    widget.setCurrentFrame(80); // Playback notifications remain ignored while scrubbing.
    mouse(widget, QEvent::MouseMove, QPointF(201, 15));
    mouse(widget, QEvent::MouseMove, QPointF(240, 15));
    mouse(widget, QEvent::MouseButtonRelease, QPointF(240, 15));
    click(widget, QPointF(760, 68)); // Existing empty-space scrub clamps at the end.
    EXPECT_EQ(frames, (std::vector<qint64>{10, 20, 99}));
    EXPECT_EQ(requests, 4);
    EXPECT_EQ(finishes, 2);
}

TEST_F(AppearanceMarkerUiTest, RefreshClampProjectResetAndSynchronousEditsKeepFrameStateCurrent) {
    std::vector<qint64> frames;
    QObject::connect(&widget, &TimelineWidget::currentFrameChanged, &widget, [&](qint64 frame) {
        frames.push_back(frame);
        // This synchronously replaces timeline storage and emits timelineChanged.
        if (frame == 40) {
            ClipAppearancePatch patch;
            patch.edgeSoftness = 0.25;
            EXPECT_TRUE(session.setClipAppearance("first", patch).changed());
        }
    });
    widget.setCurrentFrame(40);
    EXPECT_EQ(widget.currentFrame(), 40);
    widget.setFrameCount(200);
    widget.setCurrentFrame(150);
    widget.refreshProject();
    widget.refreshProject();
    EXPECT_EQ(widget.currentFrame(), 99);
    ASSERT_NO_FATAL_FAILURE(reload());
    EXPECT_EQ(widget.currentFrame(), 0);
    EXPECT_EQ(frames, (std::vector<qint64>{40, 150, 99, 0}));
}

TEST_F(AppearanceMarkerUiTest, PanelRoutingUsesPrimaryVideoAndPreservesStandaloneModalFallback) {
    ASSERT_NO_FATAL_FAILURE(selectFirst());
    click(widget, QPointF(280, 106), Qt::ControlModifier);
    widget.setClipPropertiesPanelEnabled(true);
    QStringList routed;
    int edits = 0;
    QObject::connect(&widget, &TimelineWidget::clipPropertiesRequested, &widget,
                     [&](const QString& id) { routed.push_back(id); });
    QObject::connect(&widget, &TimelineWidget::editRequested, &widget, [&] { ++edits; });
    const auto before = json();
    widget.editSelectedClipAppearance();
    EXPECT_EQ(routed, (QStringList{"second"}));
    EXPECT_EQ(json(), before);
    EXPECT_EQ(edits, 0);
    click(widget, QPointF(280, 144));
    widget.editSelectedClipAppearance();
    EXPECT_EQ(routed.size(), 1);
    ASSERT_NO_FATAL_FAILURE(selectFirst());
    widget.setClipPropertiesPanelEnabled(false);
    bool modal = false;
    QTimer::singleShot(0, &widget, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        ASSERT_NE(dialog, nullptr);
        modal = true;
        EXPECT_EQ(dialog->objectName(), "clipAppearanceDialog");
        auto* spin = dialog->findChild<QDoubleSpinBox*>("clipOpacity");
        ASSERT_NE(spin, nullptr);
        spin->setValue(25);
        dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
    });
    widget.editSelectedClipAppearance();
    EXPECT_TRUE(modal);
    EXPECT_EQ(edits, 1);
    EXPECT_EQ(routed.size(), 1);
    ASSERT_NE(session.editor().activeTimeline()->clipById("first"), nullptr);
    EXPECT_DOUBLE_EQ(session.editor().activeTimeline()->clipById("first")->opacity, 0.25);
    EXPECT_DOUBLE_EQ(session.editor().activeTimeline()->clipById("second")->opacity, 1.0);
}
} // namespace
