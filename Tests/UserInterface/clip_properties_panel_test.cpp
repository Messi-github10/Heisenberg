#include <Backend/EditorSession.hpp>
#include <MainWidget/ClipPropertiesPanel.hpp>
#include <Models/ProjectCodec.hpp>
#include <gtest/gtest.h>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QToolButton>
#include <QWheelEvent>
#include <memory>

namespace {
using namespace heisenberg;
using heisenberg::ui::EditorSession;

KeyframeTrackPayload positionTrack() {
    return {JsonValue::parse(R"({"future":"keep","keyframes":[
        {"frame":0,"value":{"a":0.2,"b":0.3,"metadata":"preserve"},"interpolationOut":"linear","id":"first"},
        {"frame":10,"value":{"a":0.8,"b":0.7},"interpolationOut":"hold","id":"last"}]})")};
}

class ClipPropertiesPanelTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(directory.isValid());
        package = directory.path() + "/properties.berg";
        ASSERT_TRUE(session.savePackage(package));
        project = session.editor().project();
        Timeline timeline;
        timeline.id = "timeline";
        Track track;
        track.id = "video";
        Clip clip;
        clip.id = "clip";
        clip.mediaRef = "offline";
        clip.startFrame = 100;
        clip.durationFrames = 30;
        clip.trimStartFrame = 17;
        clip.speed = 2;
        clip.transform.centerY = .123456789123;
        clip.transform.rotationX = 9;
        clip.effects = JsonValue::parse(R"([{"type":"custom","untouched":true}])");
        track.clips = {clip};
        auto sibling = clip;
        sibling.id = "sibling";
        sibling.startFrame = 200;
        track.clips.push_back(sibling);
        timeline.tracks = {track};
        project.timelines = {timeline};
        project.activeTimelineId = timeline.id;
        project.openTimelineIds = std::vector<std::string>{timeline.id};
        project.viewStates.reset();
        reload();
    }
    void reload() {
        QFile file(package + "/project.json");
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const auto json = ProjectCodec::encode(project);
        ASSERT_EQ(file.write(json.data(), qint64(json.size())), qint64(json.size()));
        file.close();
        QString error;
        ASSERT_TRUE(session.loadPackage(package, &error)) << error.toStdString();
    }
    Clip& stored() { return project.timelines[0].tracks[0].clips[0]; }
    Clip clip(const char* id = "clip") const { return *session.editor().activeTimeline()->clipById(id); }
    std::unique_ptr<ClipPropertiesPanel> panel(qint64 frame = 105) {
        auto result = std::make_unique<ClipPropertiesPanel>(&session);
        result->setClip("clip");
        result->setCurrentFrame(frame);
        return result;
    }
    QDoubleSpinBox* number(ClipPropertiesPanel& p, const char* name) {
        auto* result = p.findChild<QDoubleSpinBox*>(name);
        EXPECT_NE(result, nullptr) << name;
        return result;
    }
    QToolButton* button(ClipPropertiesPanel& p, const char* name) {
        auto* result = p.findChild<QToolButton*>(name);
        EXPECT_NE(result, nullptr) << name;
        return result;
    }
    EditorSession session;
    QTemporaryDir directory;
    QString package;
    ProjectFile project;
};
}

TEST_F(ClipPropertiesPanelTest, SamplesClipLocalTimeWithoutSourceTrimOrSpeedAndPreservesPrecision) {
    stored().positionTrack = positionTrack();
    reload();
    const auto before = clip();
    auto p = panel();
    EXPECT_DOUBLE_EQ(number(*p, "clipCenterX")->value(), 50);
    EXPECT_DOUBLE_EQ(number(*p, "clipCenterY")->value(), 50);
    auto* center = number(*p, "clipCenterX");
    p->setCurrentFrame(110);
    EXPECT_EQ(center, number(*p, "clipCenterX")); // Persistent editor, sampled display.
    EXPECT_DOUBLE_EQ(center->value(), 80);
    p->setSelectionCount(3);
    EXPECT_TRUE(p->findChild<QLabel*>("clipPropertiesSelectionHint")->text().contains("primary"));
    p->finishEditing();
    EXPECT_EQ(clip(), before);
    EXPECT_FALSE(session.isDirty());
}

TEST_F(ClipPropertiesPanelTest, ConstantValuesStayConstantAndAnimatedValuesUpsertOnlyTouchedComponent) {
    auto p = panel();
    const auto before = clip();
    const auto sibling = clip("sibling");
    number(*p, "clipCenterX")->setValue(25);
    p->finishEditing();
    EXPECT_DOUBLE_EQ(clip().transform.centerX, .25);
    EXPECT_DOUBLE_EQ(clip().transform.centerY, before.transform.centerY);
    EXPECT_FALSE(clip().positionTrack);
    EXPECT_EQ(clip("sibling"), sibling);
    EXPECT_EQ(clip().effects, before.effects);
    session.undo();
    EXPECT_EQ(clip(), before);
    stored().positionTrack = positionTrack();
    reload();
    p->setClip("clip");
    const auto animated = clip();
    number(*p, "clipCenterX")->setValue(65);
    p->finishEditing();
    const auto updated = clip();
    const auto& keys = updated.positionTrack->value["keyframes"];
    ASSERT_EQ(keys.size(), 3);
    EXPECT_EQ(keys[1]["frame"], 5);
    EXPECT_DOUBLE_EQ(keys[1]["value"]["a"].get<double>(), .65);
    EXPECT_DOUBLE_EQ(keys[1]["value"]["b"].get<double>(), .5);
    EXPECT_EQ(clip().positionTrack->value["future"], "keep");
    EXPECT_EQ(keys[0], animated.positionTrack->value["keyframes"][0]);
    EXPECT_EQ(keys[2], animated.positionTrack->value["keyframes"][1]);
    EXPECT_EQ(clip().transform, animated.transform);
    session.undo();
    EXPECT_EQ(clip(), animated);
}

TEST_F(ClipPropertiesPanelTest, ToggleNavigateInterpolationAndLastKeyFallback) {
    auto p = panel();
    button(*p, "clipOpacityKey")->click();
    QCoreApplication::processEvents();
    ASSERT_TRUE(clip().opacityTrack);
    EXPECT_TRUE(button(*p, "clipOpacityKey")->isChecked());
    EXPECT_TRUE(p->findChild<QLabel*>("clipOpacityHint")->text().contains("last key"));
    auto* interpolation = p->findChild<QComboBox*>("clipOpacityInterpolation");
    ASSERT_NE(interpolation, nullptr);
    EXPECT_TRUE(interpolation->isEnabled());
    interpolation->setCurrentIndex(interpolation->findData(int(Interpolation::Smooth)));
    QCoreApplication::processEvents();
    EXPECT_EQ(clip().opacityTrack->value["keyframes"][0]["interpolationOut"], "smooth");
    p->setCurrentFrame(112);
    button(*p, "clipOpacityKey")->click();
    QCoreApplication::processEvents();
    qint64 seek = -1;
    QObject::connect(p.get(), &ClipPropertiesPanel::seekFrameRequested, p.get(), [&](qint64 frame) { seek = frame; });
    button(*p, "clipOpacityPrevious")->click();
    EXPECT_EQ(seek, 105);
    p->setCurrentFrame(105);
    button(*p, "clipOpacityNext")->click();
    EXPECT_EQ(seek, 112);
    button(*p, "clipOpacityKey")->click();
    QCoreApplication::processEvents();
    p->setCurrentFrame(112);
    button(*p, "clipOpacityKey")->click();
    QCoreApplication::processEvents();
    EXPECT_FALSE(button(*p, "clipOpacityKey")->isChecked());
    EXPECT_FALSE(interpolation->isEnabled());
    EXPECT_DOUBLE_EQ(number(*p, "clipOpacity")->value(), 100);
}

TEST_F(ClipPropertiesPanelTest, OutsideRangeDisablesAnimatedWritesButAllowsConstantsAndInRangeNavigation) {
    stored().positionTrack = positionTrack();
    reload();
    auto p = panel(99);
    EXPECT_FALSE(number(*p, "clipCenterX")->isEnabled());
    EXPECT_FALSE(button(*p, "clipPositionKey")->isEnabled());
    EXPECT_TRUE(button(*p, "clipPositionNext")->isEnabled());
    EXPECT_TRUE(number(*p, "clipOpacity")->isEnabled());
    EXPECT_TRUE(number(*p, "clipEdgeSoftness")->isEnabled());
    number(*p, "clipOpacity")->setValue(75);
    p->finishEditing();
    EXPECT_DOUBLE_EQ(clip().opacity, .75);
    number(*p, "clipEdgeSoftness")->setValue(20);
    p->finishEditing();
    EXPECT_DOUBLE_EQ(clip().edgeSoftness, .2);
    p->setCurrentFrame(130);
    EXPECT_FALSE(button(*p, "clipPositionNext")->isEnabled());
    EXPECT_TRUE(button(*p, "clipPositionPrevious")->isEnabled());
    EXPECT_FALSE(button(*p, "clipPositionKey")->isEnabled());
}

TEST_F(ClipPropertiesPanelTest, ContinuousPreviewIsOneUndoAndFrameChangeFinishesFrozenTarget) {
    stored().positionTrack = positionTrack();
    reload();
    const auto before = clip();
    auto p = panel();
    int requested = 0;
    QObject::connect(p.get(), &ClipPropertiesPanel::editRequested, p.get(), [&] { ++requested; });
    auto* center = number(*p, "clipCenterX");
    center->setValue(60);
    center->setValue(70);
    center->setValue(75);
    EXPECT_EQ(requested, 1);
    center->editingFinished();
    EXPECT_TRUE(session.editor().hasPendingDocumentTransaction());
    QCoreApplication::processEvents();
    EXPECT_FALSE(session.editor().hasPendingDocumentTransaction());
    const auto edited = clip();
    session.undo();
    EXPECT_EQ(clip(), before);
    session.redo();
    EXPECT_EQ(clip(), edited);
    center->setValue(90);
    p->setCurrentFrame(110);
    EXPECT_FALSE(session.editor().hasPendingDocumentTransaction());
    EXPECT_EQ(clip().positionTrack->value["keyframes"][1]["frame"], 5);
    EXPECT_DOUBLE_EQ(clip().positionTrack->value["keyframes"][1]["value"]["a"].get<double>(), .9);
    EXPECT_DOUBLE_EQ(center->value(), 80);
}

TEST_F(ClipPropertiesPanelTest, CancelDestructionAndReturningToInitialDisplayRestoreExactPayload) {
    stored().positionTrack = positionTrack();
    reload();
    const auto before = clip();
    auto p = panel();
    auto* center = number(*p, "clipCenterX");
    const double display = center->value();
    center->setValue(77);
    p->cancelEditing();
    EXPECT_EQ(clip(), before);
    center->setValue(78);
    center->setValue(display);
    p->finishEditing();
    EXPECT_EQ(clip(), before);
    EXPECT_FALSE(session.isDirty());
    center->setValue(79);
    p.reset();
    EXPECT_EQ(clip(), before);
    EXPECT_FALSE(session.editor().hasPendingDocumentTransaction());
}

TEST_F(ClipPropertiesPanelTest, MalformedTargetDisablesOnlyItsGroupAndMetadataSurvivesOtherEdits) {
    stored().positionTrack = KeyframeTrackPayload{JsonValue::object()};
    stored().opacityTrack = KeyframeTrackPayload{JsonValue::parse(R"({"future":8,"keyframes":[{"frame":0,"value":0.5,"id":"keep","interpolationOut":"hold"}]})")};
    reload();
    const auto before = clip();
    auto p = panel(100);
    EXPECT_FALSE(number(*p, "clipCenterX")->isEnabled());
    EXPECT_FALSE(button(*p, "clipPositionKey")->isEnabled());
    EXPECT_FALSE(p->findChild<QLabel*>("clipPositionHint")->text().isEmpty());
    EXPECT_TRUE(number(*p, "clipOpacity")->isEnabled());
    EXPECT_TRUE(number(*p, "clipRotation")->isEnabled());
    number(*p, "clipOpacity")->setValue(60);
    p->finishEditing();
    EXPECT_EQ(clip().positionTrack, before.positionTrack);
    EXPECT_EQ(clip().opacityTrack->value["future"], 8);
    EXPECT_EQ(clip().opacityTrack->value["keyframes"][0]["id"], "keep");
    EXPECT_EQ(clip().opacityTrack->value["keyframes"][0]["interpolationOut"], "hold");
    EXPECT_EQ(clip().transform.rotationX, before.transform.rotationX);
}

TEST_F(ClipPropertiesPanelTest, NarrowInspectorUsesVerticalScrollWithoutHorizontalControlOverflow) {
    auto p = panel();
    p->show();
    auto* scroll = p->findChild<QScrollArea*>();
    ASSERT_NE(scroll, nullptr);
    for (const int width : {340, 400}) {
        SCOPED_TRACE(width);
        p->resize(width, 700);
        QCoreApplication::processEvents();
        EXPECT_EQ(p->width(), width);
        EXPECT_GT(scroll->verticalScrollBar()->maximum(), 0);
        EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
        const auto bounds = scroll->widget()->rect();
        for (auto* button : scroll->findChildren<QToolButton*>()) {
            const QPoint origin = button->mapTo(scroll->widget(), QPoint());
            EXPECT_GE(origin.x(), bounds.left()) << button->objectName().toStdString();
            EXPECT_LE(origin.x() + button->width(), bounds.right() + 1) << button->objectName().toStdString();
        }
        for (auto* spin : scroll->findChildren<QDoubleSpinBox*>()) {
            const QPoint origin = spin->mapTo(scroll->widget(), QPoint());
            EXPECT_GE(origin.x(), bounds.left()) << spin->objectName().toStdString();
            EXPECT_LE(origin.x() + spin->width(), bounds.right() + 1) << spin->objectName().toStdString();
        }
    }
}

TEST_F(ClipPropertiesPanelTest, InvalidSampleRecoversAtAnotherFrameUsingPreparedProperty) {
    stored().cropTrack = KeyframeTrackPayload{JsonValue::parse(R"({"keyframes":[
        {"frame":0,"value":{"left":0.37585828567644886,"top":0,"right":0.624141714323551,"bottom":0}},
        {"frame":8,"value":{"left":0.09574311346274311,"top":0,"right":0.9042568865372568,"bottom":0}}]})")};
    reload();
    auto p = panel(104);
    EXPECT_FALSE(number(*p, "clipCropLeft")->isEnabled());
    EXPECT_FALSE(p->findChild<QLabel*>("clipCropHint")->text().isEmpty());
    EXPECT_TRUE(number(*p, "clipOpacity")->isEnabled());
    p->setCurrentFrame(100);
    EXPECT_TRUE(number(*p, "clipCropLeft")->isEnabled());
    EXPECT_TRUE(button(*p, "clipCropKey")->isEnabled());
    EXPECT_TRUE(button(*p, "clipCropKey")->isChecked());
    p->setCurrentFrame(104);
    EXPECT_FALSE(number(*p, "clipCropLeft")->isEnabled());
    p->setCurrentFrame(108);
    EXPECT_TRUE(number(*p, "clipCropLeft")->isEnabled());
    EXPECT_FALSE(session.isDirty());
}

TEST_F(ClipPropertiesPanelTest, HorizontalScrubCommitsOnceAndEscapeStopsFurtherDragUpdates) {
    auto p = panel();
    p->resize(440, 700);
    p->show();
    QCoreApplication::processEvents();
    auto* handle = button(*p, "clipCenterXScrub");
    auto* center = number(*p, "clipCenterX");
    const auto before = clip();
    const QPointF local(handle->rect().center());
    const QPointF global(handle->mapToGlobal(local.toPoint()));
    auto mouse = [&](QEvent::Type type, int delta, Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, local + QPointF(delta, 0), global + QPointF(delta, 0),
                          button, buttons, Qt::NoModifier);
        QApplication::sendEvent(handle, &event);
    };
    int requests = 0;
    QObject::connect(p.get(), &ClipPropertiesPanel::editRequested, p.get(), [&] { ++requests; });
    mouse(QEvent::MouseButtonPress, 0, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseMove, 5, Qt::NoButton, Qt::LeftButton);
    mouse(QEvent::MouseMove, 12, Qt::NoButton, Qt::LeftButton);
    EXPECT_EQ(requests, 1);
    EXPECT_TRUE(session.editor().hasPendingDocumentTransaction());
    mouse(QEvent::MouseButtonRelease, 12, Qt::LeftButton, Qt::NoButton);
    QCoreApplication::processEvents();
    EXPECT_FALSE(session.editor().hasPendingDocumentTransaction());
    EXPECT_DOUBLE_EQ(clip().transform.centerX, .62);
    const auto edited = clip();
    session.undo();
    EXPECT_EQ(clip(), before);
    session.redo();
    EXPECT_EQ(clip(), edited);
    mouse(QEvent::MouseButtonPress, 0, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseMove, 8, Qt::NoButton, Qt::LeftButton);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(center, &escape);
    QCoreApplication::processEvents();
    EXPECT_EQ(clip(), edited);
    mouse(QEvent::MouseMove, 20, Qt::NoButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, 20, Qt::LeftButton, Qt::NoButton);
    QCoreApplication::processEvents();
    EXPECT_EQ(clip(), edited);
    EXPECT_FALSE(session.editor().hasPendingDocumentTransaction());
}

TEST_F(ClipPropertiesPanelTest, RepeatedKeyboardStepsCommitOnFinalReleaseAsOneUndo) {
    auto p = panel();
    auto* center = number(*p, "clipCenterX");
    const auto before = clip();
    int requests = 0;
    QObject::connect(p.get(), &ClipPropertiesPanel::editRequested, p.get(), [&] { ++requests; });
    for (int i = 0; i < 4; ++i) {
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier, QString(), i != 0);
        QApplication::sendEvent(center, &press);
        if (i < 3) {
            QKeyEvent release(QEvent::KeyRelease, Qt::Key_Up, Qt::NoModifier, QString(), true);
            QApplication::sendEvent(center, &release);
        }
    }
    EXPECT_EQ(requests, 1);
    EXPECT_TRUE(session.editor().hasPendingDocumentTransaction());
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Up, Qt::NoModifier);
    QApplication::sendEvent(center, &release);
    QCoreApplication::processEvents();
    EXPECT_FALSE(session.editor().hasPendingDocumentTransaction());
    EXPECT_GT(clip().transform.centerX, before.transform.centerX);
    session.undo();
    EXPECT_EQ(clip(), before);
}

TEST_F(ClipPropertiesPanelTest, KeyboardTrackingWheelEscapeAndControlShortcutBoundaries) {
    auto p = panel();
    p->resize(450, 900);
    p->show();
    QCoreApplication::processEvents();
    auto* center = number(*p, "clipCenterX");
    ASSERT_FALSE(center->keyboardTracking());
    const auto before = clip();
    center->setFocus();
    auto* line = center->findChild<QLineEdit*>();
    ASSERT_NE(line, nullptr);
    line->setText("73");
    EXPECT_EQ(clip(), before);
    p->finishEditing();
    EXPECT_DOUBLE_EQ(clip().transform.centerX, .73);
    const auto committed = clip();
    QWheelEvent wheel(QPointF(10, 10), center->mapToGlobal(QPoint(10, 10)), QPoint(), QPoint(0, 120),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(center, &wheel);
    EXPECT_EQ(clip(), committed);
    center->setValue(80);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(center, &escape);
    QCoreApplication::processEvents();
    EXPECT_EQ(clip(), committed);
    center->setValue(85);
    QKeyEvent shortcut(QEvent::ShortcutOverride, Qt::Key_S, Qt::ControlModifier);
    QApplication::sendEvent(center, &shortcut);
    EXPECT_FALSE(session.editor().hasPendingDocumentTransaction());
    EXPECT_DOUBLE_EQ(clip().transform.centerX, .85);
}
