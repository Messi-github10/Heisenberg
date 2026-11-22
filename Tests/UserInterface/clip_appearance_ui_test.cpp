#include <Backend/EditorSession.hpp>
#include <MainWidget/ClipAppearanceDialog.hpp>
#include <MainWidget/TimelineWidget.hpp>
#include <Models/BlendMode.hpp>
#include <Models/ProjectCodec.hpp>

#include <gtest/gtest.h>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <functional>
#include <limits>

namespace {
using namespace heisenberg;
using namespace heisenberg::ui;

QDoubleSpinBox* field(QDialog& dialog, const char* name) {
    auto* spin = dialog.findChild<QDoubleSpinBox*>(name);
    EXPECT_NE(spin, nullptr) << name;
    return spin;
}

QComboBox* blendField(QDialog& dialog) {
    auto* combo = dialog.findChild<QComboBox*>("clipBlendMode");
    EXPECT_NE(combo, nullptr);
    return combo;
}

TEST(ClipAppearanceDialogTest, BlendModesUseSharedTableAndOnlySubmitCanonicalChanges) {
    const auto modes = blendModes();
    for (const auto& descriptor : modes) {
        Clip clip;
        clip.blendMode = std::string(descriptor.name);
        ClipAppearanceDialog dialog(clip);
        auto* combo = blendField(dialog);
        ASSERT_NE(combo, nullptr);
        ASSERT_EQ(combo->count(), static_cast<int>(modes.size()));
        EXPECT_FALSE(combo->isEditable());
        EXPECT_EQ(combo->currentData().toString().toStdString(), *clip.blendMode);
        EXPECT_TRUE(dialog.changedPatch().empty());
        for (std::size_t i = 0; i < modes.size(); ++i) {
            EXPECT_EQ(combo->itemText(static_cast<int>(i)).toStdString(), modes[i].displayName);
            EXPECT_EQ(combo->itemData(static_cast<int>(i)).toString().toStdString(), modes[i].name);
            combo->setCurrentIndex(static_cast<int>(i));
            const auto patch = dialog.changedPatch();
            if (modes[i].mode == descriptor.mode) EXPECT_TRUE(patch.empty());
            else EXPECT_EQ(patch.blendMode, std::optional<std::string>{std::string(modes[i].name)});
        }
    }
}

TEST(ClipAppearanceDialogTest, AbsentNormalAndUnsupportedValuesStayOmittedUntilChanged) {
    for (const auto& stored : {std::optional<std::string>{}, std::optional<std::string>{"normal"},
                              std::optional<std::string>{"futureBlend"}, std::optional<std::string>{""}}) {
        Clip clip;
        clip.blendMode = stored;
        ClipAppearanceDialog dialog(clip);
        auto* combo = blendField(dialog);
        ASSERT_NE(combo, nullptr);
        const int originalIndex = combo->currentIndex();
        const bool known = parseBlendMode(stored).has_value();
        EXPECT_EQ(combo->count(), static_cast<int>(blendModes().size()) + (known ? 0 : 1));
        if (!known) {
            EXPECT_TRUE(combo->currentText().startsWith("Unsupported"));
            EXPECT_FALSE(combo->currentData().isValid());
        } else {
            EXPECT_EQ(combo->currentData().toString(), "normal");
        }
        EXPECT_TRUE(dialog.changedPatch().empty());
        combo->setCurrentIndex(combo->findData("screen"));
        EXPECT_EQ(dialog.changedPatch().blendMode, std::optional<std::string>{"screen"});
        combo->setCurrentIndex(originalIndex);
        EXPECT_TRUE(dialog.changedPatch().empty());
        field(dialog, "clipOpacity")->setValue(25);
        EXPECT_FALSE(dialog.changedPatch().blendMode);
        EXPECT_TRUE(dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->isEnabled());
        if (!known) {
            combo->setCurrentIndex(combo->findData("normal"));
            EXPECT_EQ(dialog.changedPatch().blendMode, std::optional<std::string>{"normal"});
        }
    }
}

TEST(ClipAppearanceDialogTest, UsesPercentAndDegreeUnitsAndOnlyReturnsChangedControls) {
    Clip clip;
    clip.transform.centerX = 0.123456789123;
    clip.transform.centerY = -2.5;
    clip.transform.width = 1.23456789123;
    clip.transform.rotation = 450.0;
    clip.opacity = 0.987654321;
    ClipAppearanceDialog dialog(clip);
    EXPECT_DOUBLE_EQ(field(dialog, "clipCenterY")->value(), -250.0);
    EXPECT_DOUBLE_EQ(field(dialog, "clipRotation")->value(), 450.0);
    EXPECT_TRUE(dialog.changedPatch().empty());
    auto* center = field(dialog, "clipCenterX");
    const auto originalDisplay = center->value();
    center->setValue(25.0);
    auto patch = dialog.changedPatch();
    ASSERT_TRUE(patch.centerX);
    EXPECT_DOUBLE_EQ(*patch.centerX, 0.25);
    EXPECT_FALSE(patch.width);
    EXPECT_FALSE(patch.opacity);
    center->setValue(originalDisplay);
    EXPECT_TRUE(dialog.changedPatch().empty());
    auto* flip = dialog.findChild<QCheckBox*>("clipFlipHorizontal");
    ASSERT_NE(flip, nullptr);
    flip->setChecked(true);
    patch = dialog.changedPatch();
    ASSERT_TRUE(patch.flipHorizontal);
    EXPECT_TRUE(*patch.flipHorizontal);
    EXPECT_FALSE(patch.flipVertical);
}

TEST(ClipAppearanceDialogTest, UntouchedExtremeAndOutOfDomainImportedValuesAreNotSubmitted) {
    Clip clip;
    clip.transform.centerX = std::numeric_limits<double>::max();
    clip.transform.centerY = -1e250;
    clip.transform.width = -5.0;
    clip.transform.height = 1e200;
    clip.transform.rotation = 1e200;
    clip.crop.left = 5.0;
    clip.crop.bottom = -4.0;
    clip.edgeRounding = -2.0;
    clip.edgeSoftness = 3.0;
    ClipAppearanceDialog dialog(clip);
    EXPECT_TRUE(dialog.changedPatch().empty());
    EXPECT_DOUBLE_EQ(field(dialog, "clipWidth")->value(), -500.0);
    EXPECT_DOUBLE_EQ(field(dialog, "clipCropLeft")->value(), 500.0);
    field(dialog, "clipOpacity")->setValue(25.0);
    const auto patch = dialog.changedPatch();
    EXPECT_FALSE(patch.centerX);
    EXPECT_FALSE(patch.centerY);
    EXPECT_FALSE(patch.width);
    EXPECT_FALSE(patch.height);
    EXPECT_FALSE(patch.rotation);
    EXPECT_FALSE(patch.cropLeft);
    EXPECT_FALSE(patch.cropBottom);
    EXPECT_FALSE(patch.edgeRounding);
    EXPECT_FALSE(patch.edgeSoftness);
    ASSERT_TRUE(patch.opacity);
    EXPECT_DOUBLE_EQ(*patch.opacity, 0.25);
}

class ClipAppearanceUiTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(directory.isValid());
        package = directory.path() + "/appearance.berg";
        ASSERT_TRUE(session.savePackage(package));
        project = session.editor().project();
        Timeline active;
        active.id = "timeline";
        Track video;
        video.id = "video";
        Clip clip;
        clip.id = "clip";
        clip.mediaRef = "media";
        clip.durationFrames = 30;
        clip.transform.centerX = 0.123456789123;
        clip.transform.rotationX = 10.0;
        clip.transform.rotationY = -15.0;
        clip.opacityTrack = KeyframeTrackPayload{JsonValue{{"opaque", true}}};
        video.clips = {clip};
        active.tracks = {video};
        project.timelines = {active};
        project.activeTimelineId = active.id;
        project.openTimelineIds = std::vector<std::string>{active.id};
        project.viewStates.reset();
        auto manifest = session.editor().manifest();
        MediaManifestEntry entry;
        entry.id = "media";
        entry.type = ClipType::Video;
        entry.duration = 10.0;
        entry.source = {MediaSource::Kind::External, directory.path().toStdString() + "/offline.mp4"};
        ASSERT_TRUE(manifest.add(entry));
        writeDocument("media.json", manifest.toJson());
        reload();
        widget.setSession(&session);
        widget.resize(800, 240);
        widget.show();
        selectClip();
    }
    void clickClip(const QPointF& point, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QCoreApplication::processEvents();
        QMouseEvent press(QEvent::MouseButtonPress, point, widget.mapToGlobal(point.toPoint()),
                          Qt::LeftButton, Qt::LeftButton, modifiers);
        QApplication::sendEvent(&widget, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, point, widget.mapToGlobal(point.toPoint()),
                            Qt::LeftButton, Qt::NoButton, modifiers);
        QApplication::sendEvent(&widget, &release);
    }
    void selectClip() {
        clickClip(QPointF(200, 68));
        ASSERT_EQ(widget.selectedClipId(), "clip");
    }
    void writeDocument(const QString& name, const std::string& json) {
        QFile file(package + '/' + name);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        ASSERT_EQ(file.write(json.data(), static_cast<qint64>(json.size())), static_cast<qint64>(json.size()));
    }
    void reload() {
        writeDocument("project.json", ProjectCodec::encode(project));
        QString error;
        ASSERT_TRUE(session.loadPackage(package, &error)) << error.toStdString();
    }
    void appearance(const std::function<void(QDialog&)>& modify, bool accept = true) {
        QTimer::singleShot(0, &widget, [modify, accept] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) { ADD_FAILURE() << "Appearance dialog missing"; return; }
            EXPECT_EQ(dialog->objectName(), "clipAppearanceDialog");
            if (modify) modify(*dialog);
            if (accept) dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
            else dialog->reject();
        });
        widget.editSelectedClipAppearance();
    }
    std::string json() const { return ProjectCodec::encode(session.editor().project()); }
    const Clip& clip() const { return *session.editor().activeTimeline()->clipById("clip"); }
    QTemporaryDir directory;
    EditorSession session;
    TimelineWidget widget;
    ProjectFile project;
    QString package;
};

TEST_F(ClipAppearanceUiTest, CancelNoOpAndChangeBackDoNotEmitOrDirty) {
    const auto before = json();
    const auto revision = session.editor().revision();
    int edits = 0, changes = 0;
    QObject::connect(&widget, &TimelineWidget::editRequested, &widget, [&] { ++edits; });
    QObject::connect(&session, &EditorSession::timelineChanged, &widget, [&](qint64) { ++changes; });
    appearance([](QDialog& dialog) {
        field(dialog, "clipOpacity")->setValue(25);
        blendField(dialog)->setCurrentIndex(blendField(dialog)->findData("screen"));
    }, false);
    appearance({});
    appearance([](QDialog& dialog) {
        auto* spin = field(dialog, "clipCenterX");
        const auto initial = spin->value();
        spin->setValue(50);
        spin->setValue(initial);
        auto* blend = blendField(dialog);
        const int originalBlend = blend->currentIndex();
        blend->setCurrentIndex(blend->findData("multiply"));
        blend->setCurrentIndex(originalBlend);
    });
    EXPECT_EQ(json(), before);
    EXPECT_EQ(session.editor().revision(), revision);
    EXPECT_EQ(edits, 0);
    EXPECT_EQ(changes, 0);
    EXPECT_FALSE(session.isDirty());
}

TEST_F(ClipAppearanceUiTest, MultipleFieldsCommitOnceAndUndoPreservesPayloads) {
    const auto before = json();
    const auto original = clip();
    int changes = 0;
    QObject::connect(&session, &EditorSession::timelineChanged, &widget, [&](qint64) { ++changes; });
    appearance([](QDialog& dialog) {
        field(dialog, "clipCenterY")->setValue(75);
        field(dialog, "clipWidth")->setValue(125);
        field(dialog, "clipHeight")->setValue(80);
        field(dialog, "clipRotation")->setValue(45);
        field(dialog, "clipCropLeft")->setValue(10);
        field(dialog, "clipCropTop")->setValue(20);
        field(dialog, "clipCropRight")->setValue(30);
        field(dialog, "clipCropBottom")->setValue(40);
        field(dialog, "clipEdgeRounding")->setValue(15);
        field(dialog, "clipEdgeSoftness")->setValue(25);
        field(dialog, "clipOpacity")->setValue(35);
        blendField(dialog)->setCurrentIndex(blendField(dialog)->findData("colorBurn"));
        dialog.findChild<QCheckBox*>("clipFlipHorizontal")->setChecked(true);
        dialog.findChild<QCheckBox*>("clipFlipVertical")->setChecked(true);
    });
    EXPECT_EQ(changes, 1);
    EXPECT_TRUE(session.isDirty());
    auto expected = original;
    expected.transform.centerY = 0.75;
    expected.transform.width = 1.25;
    expected.transform.height = 0.8;
    expected.transform.rotation = 45;
    expected.transform.flipHorizontal = expected.transform.flipVertical = true;
    expected.crop = {0.1, 0.2, 0.3, 0.4};
    expected.edgeRounding = 0.15;
    expected.edgeSoftness = 0.25;
    expected.opacity = 0.35;
    expected.blendMode = "colorBurn";
    EXPECT_EQ(clip(), expected);
    const auto after = json();
    session.undo();
    EXPECT_EQ(json(), before);
    session.redo();
    EXPECT_EQ(json(), after);
}

TEST_F(ClipAppearanceUiTest, MultiSelectionAppearanceEditsOnlyThePrimaryClip) {
    Clip secondary = project.timelines[0].tracks[0].clips[0];
    secondary.id = "secondary";
    secondary.startFrame = 40;
    secondary.opacity = 0.8;
    secondary.transform.width = 1.4;
    secondary.blendMode = "screen";
    project.timelines[0].tracks[0].clips.push_back(secondary);
    reload();
    selectClip();
    clickClip(QPointF(360, 68), Qt::ControlModifier);
    auto selected = widget.selectedClipIds();
    selected.sort();
    ASSERT_EQ(selected, (QStringList{"clip", "secondary"}));
    ASSERT_EQ(widget.selectedClipId(), "secondary");
    const auto before = session.editor().project();
    const auto revision = session.editor().revision();
    int changes = 0;
    QObject::connect(&session, &EditorSession::timelineChanged, &widget, [&](qint64) { ++changes; });

    appearance([](QDialog& dialog) {
        EXPECT_DOUBLE_EQ(field(dialog, "clipOpacity")->value(), 80);
        EXPECT_DOUBLE_EQ(field(dialog, "clipWidth")->value(), 140);
        EXPECT_EQ(blendField(dialog)->currentData().toString(), "screen");
        blendField(dialog)->setCurrentIndex(blendField(dialog)->findData("multiply"));
        field(dialog, "clipOpacity")->setValue(35);
        field(dialog, "clipWidth")->setValue(150);
    });

    auto expected = before;
    auto& primary = *expected.timelines[0].clipById("secondary");
    primary.opacity = 0.35;
    primary.transform.width = 1.5;
    primary.blendMode = "multiply";
    EXPECT_EQ(session.editor().project(), expected);
    EXPECT_EQ(session.editor().revision(), revision + 1);
    EXPECT_EQ(changes, 1);
    EXPECT_EQ(widget.selectedClipId(), "secondary");
    auto selectedAfter = widget.selectedClipIds();
    selectedAfter.sort();
    EXPECT_EQ(selectedAfter, selected);
    session.undo();
    EXPECT_EQ(session.editor().project(), before);
    session.redo();
    EXPECT_EQ(session.editor().project(), expected);
}

TEST_F(ClipAppearanceUiTest, SavedAndReopenedPackagePreservesAllEditedConstantsAndOpaquePayloads) {
    const auto original = clip();
    appearance([](QDialog& dialog) {
        field(dialog, "clipCenterX")->setValue(-25);
        field(dialog, "clipCenterY")->setValue(75);
        field(dialog, "clipWidth")->setValue(125);
        field(dialog, "clipHeight")->setValue(80);
        field(dialog, "clipRotation")->setValue(405);
        field(dialog, "clipCropLeft")->setValue(10);
        field(dialog, "clipCropTop")->setValue(20);
        field(dialog, "clipCropRight")->setValue(30);
        field(dialog, "clipCropBottom")->setValue(40);
        field(dialog, "clipEdgeRounding")->setValue(15);
        field(dialog, "clipEdgeSoftness")->setValue(25);
        field(dialog, "clipOpacity")->setValue(35);
        blendField(dialog)->setCurrentIndex(blendField(dialog)->findData("colorBurn"));
        dialog.findChild<QCheckBox*>("clipFlipHorizontal")->setChecked(true);
        dialog.findChild<QCheckBox*>("clipFlipVertical")->setChecked(true);
    });
    auto expected = original;
    expected.transform.centerX = -0.25;
    expected.transform.centerY = 0.75;
    expected.transform.width = 1.25;
    expected.transform.height = 0.8;
    expected.transform.rotation = 405;
    expected.transform.flipHorizontal = expected.transform.flipVertical = true;
    expected.crop = {0.1, 0.2, 0.3, 0.4};
    expected.edgeRounding = 0.15;
    expected.edgeSoftness = 0.25;
    expected.opacity = 0.35;
    expected.blendMode = "colorBurn";
    ASSERT_EQ(clip(), expected);
    ASSERT_TRUE(session.isDirty());

    QString error;
    ASSERT_TRUE(session.savePackage(package, &error)) << error.toStdString();
    EXPECT_FALSE(session.isDirty());
    const auto saved = session.editor().project();
    EditorSession reopened;
    ASSERT_TRUE(reopened.loadPackage(package, &error)) << error.toStdString();
    ASSERT_NE(reopened.editor().activeTimeline(), nullptr);
    const auto* restored = reopened.editor().activeTimeline()->clipById("clip");
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(*restored, expected);
    EXPECT_EQ(reopened.editor().project(), saved);
    EXPECT_FALSE(reopened.isDirty());
}

TEST_F(ClipAppearanceUiTest, InvalidMergedCropKeepsDraftOpenUntilCorrected) {
    const auto before = json();
    appearance([](QDialog& dialog) {
        field(dialog, "clipWidth")->setValue(200);
        field(dialog, "clipCropLeft")->setValue(60);
        field(dialog, "clipCropRight")->setValue(40);
        auto* ok = dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
        auto* error = dialog.findChild<QLabel*>("clipAppearanceValidation");
        ASSERT_NE(error, nullptr);
        EXPECT_FALSE(ok->isEnabled());
        EXPECT_FALSE(error->text().isEmpty());
        dialog.accept();
        EXPECT_TRUE(dialog.isVisible());
        EXPECT_DOUBLE_EQ(field(dialog, "clipWidth")->value(), 200);
        field(dialog, "clipCropRight")->setValue(30);
        EXPECT_TRUE(ok->isEnabled());
        EXPECT_TRUE(error->text().isEmpty());
    });
    EXPECT_DOUBLE_EQ(clip().transform.width, 2.0);
    EXPECT_DOUBLE_EQ(clip().crop.left, 0.6);
    EXPECT_DOUBLE_EQ(clip().crop.right, 0.3);
    session.undo();
    EXPECT_EQ(json(), before);
}

TEST_F(ClipAppearanceUiTest, UntouchedUnusualGeometrySurvivesOpacityChange) {
    auto& imported = project.timelines[0].tracks[0].clips[0];
    imported.transform.centerX = std::numeric_limits<double>::max();
    imported.transform.width = -2.0;
    imported.transform.rotation = 1e200;
    imported.crop.left = 1.5;
    imported.edgeSoftness = -0.2;
    reload();
    selectClip();
    auto expected = clip();
    appearance({});
    EXPECT_FALSE(session.isDirty());
    appearance([](QDialog& dialog) { field(dialog, "clipOpacity")->setValue(25); });
    expected.opacity = 0.25;
    EXPECT_EQ(clip(), expected);
}

TEST_F(ClipAppearanceUiTest, ImportedUnknownBlendCanBePreservedRepairedAndSaved) {
    for (const auto* name : {"futureBlend", "", "Normal"}) {
        SCOPED_TRACE(name);
        project.timelines[0].tracks[0].clips[0].blendMode = name;
        reload();
        selectClip();
        const auto before = json();
        const auto revision = session.editor().revision();
        appearance({});
        appearance([](QDialog& dialog) {
            auto* combo = blendField(dialog);
            const int initial = combo->currentIndex();
            combo->setCurrentIndex(combo->findData("screen"));
            combo->setCurrentIndex(initial);
        });
        EXPECT_EQ(json(), before);
        EXPECT_EQ(session.editor().revision(), revision);
        EXPECT_FALSE(session.isDirty());
        appearance([](QDialog& dialog) { field(dialog, "clipOpacity")->setValue(25); });
        EXPECT_EQ(clip().blendMode, std::optional<std::string>{name});
        EXPECT_DOUBLE_EQ(clip().opacity, 0.25);
        const auto preserved = session.editor().project();
        QString error;
        ASSERT_TRUE(session.savePackage(package, &error)) << error.toStdString();
        EditorSession reopened;
        ASSERT_TRUE(reopened.loadPackage(package, &error)) << error.toStdString();
        EXPECT_EQ(reopened.editor().project(), preserved);
        appearance([](QDialog& dialog) {
            auto* combo = blendField(dialog);
            combo->setCurrentIndex(combo->findData("normal"));
        });
        EXPECT_EQ(clip().blendMode, std::optional<std::string>{"normal"});
        const auto repaired = session.editor().project();
        session.undo();
        EXPECT_EQ(session.editor().project(), preserved);
        session.redo();
        EXPECT_EQ(session.editor().project(), repaired);
        ASSERT_TRUE(session.savePackage(package, &error)) << error.toStdString();
        ASSERT_TRUE(reopened.loadPackage(package, &error)) << error.toStdString();
        EXPECT_EQ(reopened.editor().project(), repaired);
    }
}

TEST_F(ClipAppearanceUiTest, FlushesPendingGradeBeforeApplyingPatchToResolvedClip) {
    const auto before = json();
    ASSERT_TRUE(session.beginClipEffectsTransaction("clip").message.empty());
    const JsonValue effects = JsonValue::array({{{"type", "future.grade"}, {"value", 1.5}}});
    ASSERT_TRUE(session.updateClipEffectsTransaction(effects).changed());
    int flushes = 0;
    QObject::connect(&widget, &TimelineWidget::editRequested, &widget, [&] {
        ++flushes;
        EXPECT_TRUE(session.commitClipEffectsTransaction().changed());
    });
    appearance([](QDialog& dialog) {
        field(dialog, "clipWidth")->setValue(150);
        blendField(dialog)->setCurrentIndex(blendField(dialog)->findData("softLight"));
    });
    EXPECT_EQ(flushes, 1);
    EXPECT_EQ(clip().blendMode, std::optional<std::string>{"softLight"});
    EXPECT_DOUBLE_EQ(clip().transform.width, 1.5);
    EXPECT_EQ(clip().effects, effects);
    session.undo();
    EXPECT_DOUBLE_EQ(clip().transform.width, 1.0);
    EXPECT_FALSE(clip().blendMode);
    EXPECT_EQ(clip().effects, effects);
    session.undo();
    EXPECT_EQ(json(), before);
}

TEST_F(ClipAppearanceUiTest, SessionReplacementDuringEditRequestCannotApplyToStaleClip) {
    const auto before = json();
    EditorSession replacement;
    QObject::connect(&widget, &TimelineWidget::editRequested, &widget, [&] { widget.setSession(&replacement); });
    appearance([](QDialog& dialog) { field(dialog, "clipOpacity")->setValue(25); });
    EXPECT_EQ(json(), before);
    EXPECT_FALSE(session.isDirty());
    EXPECT_FALSE(replacement.isDirty());
    widget.setSession(nullptr);
}
} // namespace
