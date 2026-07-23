/*
 * test_properties.cpp — Tests for Step 17: Properties Panel
 *
 * Tests ScrubbySpinBox and PropertiesPanel with all clip types.
 */

#include <gtest/gtest.h>

#include "widgets/ScrubbySpinBox.h"
#include "widgets/KeyboardFocusUtils.h"
#include "panels/properties/PropertiesPanel.h"
#include "panels/effects/EffectControlsPanel.h"
#include "panels/effects/GraphicsEditorPanel.h"
#include "panels/effects/KeyframeEditor.h"
#include "panels/timeline/TimelinePanel.h"

#include "timeline/Clip.h"
#include "timeline/SpineClip.h"
#include "timeline/VideoClip.h"
#include "timeline/AudioClip.h"
#include "timeline/GraphicClip.h"
#include "timeline/TitleClip.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "command/CommandStack.h"
#include "effects/Blur.h"
#include "effects/Tint.h"

#include <QApplication>
#include <QSignalSpy>
#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDialog>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QToolButton>
#include <QTest>
#include <QTimer>

#include <algorithm>
#include <memory>

TEST(GraphicsEditorPanel, TextRowsAreMultilineAndFontFamilyIsSearchable)
{
    rt::GraphicClip clip;
    clip.setDuration(48000);
    auto* layer = clip.addTextLayer("alpha");
    ASSERT_NE(layer, nullptr);

    rt::GraphicsEditorPanel panel;
    panel.setClip(&clip);

    QPlainTextEdit* textEdit = nullptr;
    for (auto* candidate : panel.findChildren<QPlainTextEdit*>()) {
        if (candidate->toPlainText() == QStringLiteral("alpha")) {
            textEdit = candidate;
            break;
        }
    }
    ASSERT_NE(textEdit, nullptr);
    textEdit->setReadOnly(false);
    textEdit->setTextInteractionFlags(Qt::TextEditorInteraction);
    QTextCursor cursor = textEdit->textCursor();
    cursor.movePosition(QTextCursor::End);
    textEdit->setTextCursor(cursor);
    QKeyEvent returnPress(QEvent::KeyPress, Qt::Key_Return,
                          Qt::NoModifier);
    QApplication::sendEvent(textEdit, &returnPress);
    EXPECT_EQ(textEdit->toPlainText(), QStringLiteral("alpha\n"));
    EXPECT_EQ(layer->text(), "alpha\n");

    auto* fontCombo = panel.findChild<QComboBox*>(
        QStringLiteral("graphicsFontFamilyCombo"));
    ASSERT_NE(fontCombo, nullptr);
    EXPECT_TRUE(fontCombo->isEditable());
    ASSERT_NE(fontCombo->completer(), nullptr);
    EXPECT_EQ(fontCombo->completer()->caseSensitivity(),
              Qt::CaseInsensitive);
    EXPECT_EQ(fontCombo->completer()->filterMode(), Qt::MatchContains);

    // Keep one real face/style selector; the generic weight dropdown used to
    // duplicate choices such as Regular and Bold.
    auto* styleCombo = panel.findChild<QComboBox*>(
        QStringLiteral("graphicsFontStyleCombo"));
    ASSERT_NE(styleCombo, nullptr);
    for (auto* combo : panel.findChildren<QComboBox*>()) {
        const bool isOldWeightMenu = combo->findText(QStringLiteral("Thin")) >= 0
            && combo->findText(QStringLiteral("Semi-Bold")) >= 0
            && combo->findText(QStringLiteral("Black")) >= 0;
        EXPECT_FALSE(isOldWeightMenu);
    }
    EXPECT_TRUE(rt::widgetConsumesTextKeys(fontCombo));
    EXPECT_TRUE(rt::widgetConsumesTextKeys(fontCombo->lineEdit()));
}

// ═══════════════════════════════════════════════════════════════════════════
//  QApplication fixture
// ═══════════════════════════════════════════════════════════════════════════

static int    s_argc = 1;
static char   s_arg0[] = "test_properties";
static char*  s_argv[] = { s_arg0, nullptr };

class PropertiesTestEnv : public ::testing::Environment
{
public:
    void SetUp() override
    {
        if (!QApplication::instance())
            m_app = new QApplication(s_argc, s_argv);
    }
    void TearDown() override {}
private:
    QApplication* m_app{nullptr};
};

static auto* g_env = ::testing::AddGlobalTestEnvironment(new PropertiesTestEnv);

class DeleteProbeWidget : public QWidget
{
public:
    int deletePresses{0};

protected:
    void keyPressEvent(QKeyEvent* event) override
    {
        if (event->key() == Qt::Key_Delete
            || event->key() == Qt::Key_Backspace) {
            ++deletePresses;
        }
        QWidget::keyPressEvent(event);
    }
};

static rt::PropertyRow* findEffectPropertyRow(rt::EffectControlsPanel& panel,
                                              const QString& name)
{
    for (auto* row : panel.propertyRows())
        if (row && row->propertyName() == name) return row;
    return nullptr;
}

static QToolButton* findKeyframeDiamond(rt::PropertyRow* row)
{
    if (!row) return nullptr;
    for (auto* button : row->findChildren<QToolButton*>())
        if (button->text() == QStringLiteral("\u25C6")) return button;
    return nullptr;
}

// ═══════════════════════════════════════════════════════════════════════════
//  ScrubbySpinBox tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(ScrubbySpinBox, DefaultConstruction)
{
    rt::ScrubbySpinBox spin;
    EXPECT_FALSE(spin.isScrubbing());
    EXPECT_DOUBLE_EQ(spin.scrubStep(), 0.01);
    EXPECT_DOUBLE_EQ(spin.fineMultiplier(), 0.1);
    EXPECT_DOUBLE_EQ(spin.coarseMultiplier(), 10.0);
}

TEST(ScrubbySpinBox, SetScrubStep)
{
    rt::ScrubbySpinBox spin;
    spin.setScrubStep(0.5);
    EXPECT_DOUBLE_EQ(spin.scrubStep(), 0.5);
}

TEST(ScrubbySpinBox, SetMultipliers)
{
    rt::ScrubbySpinBox spin;
    spin.setFineMultiplier(0.05);
    spin.setCoarseMultiplier(20.0);
    EXPECT_DOUBLE_EQ(spin.fineMultiplier(), 0.05);
    EXPECT_DOUBLE_EQ(spin.coarseMultiplier(), 20.0);
}

TEST(ScrubbySpinBox, IntegerMode)
{
    rt::ScrubbySpinBox spin;
    spin.setIntegerMode();
    EXPECT_EQ(spin.decimals(), 0);
    EXPECT_DOUBLE_EQ(spin.scrubStep(), 1.0);
    EXPECT_DOUBLE_EQ(spin.singleStep(), 1.0);
}

TEST(ScrubbySpinBox, IntValueConvenience)
{
    rt::ScrubbySpinBox spin;
    spin.setIntegerMode();
    spin.setRange(0, 100);
    spin.setIntValue(42);
    EXPECT_EQ(spin.intValue(), 42);
}

TEST(ScrubbySpinBox, RangeAndValue)
{
    rt::ScrubbySpinBox spin;
    spin.setRange(-100.0, 100.0);
    spin.setValue(50.5);
    EXPECT_DOUBLE_EQ(spin.value(), 50.5);
    EXPECT_DOUBLE_EQ(spin.minimum(), -100.0);
    EXPECT_DOUBLE_EQ(spin.maximum(), 100.0);
}

TEST(ScrubbySpinBox, Decimals)
{
    rt::ScrubbySpinBox spin;
    spin.setDecimals(4);
    EXPECT_EQ(spin.decimals(), 4);
}

TEST(ScrubbySpinBox, Suffix)
{
    rt::ScrubbySpinBox spin;
    spin.setSuffix(" px");
    EXPECT_EQ(spin.suffix(), QString(" px"));
}

TEST(ScrubbySpinBox, ScrubStartValueInitial)
{
    rt::ScrubbySpinBox spin;
    EXPECT_DOUBLE_EQ(spin.scrubStartValue(), 0.0);
}

// ═══════════════════════════════════════════════════════════════════════════
//  PropertiesPanel — construction
// ═══════════════════════════════════════════════════════════════════════════

TEST(PropertiesPanel, DefaultConstruction)
{
    rt::PropertiesPanel panel;
    EXPECT_EQ(panel.clip(), nullptr);
    EXPECT_EQ(panel.track(), nullptr);
    EXPECT_EQ(panel.commandStack(), nullptr);

    // All widget accessors should return non-null
    EXPECT_NE(panel.labelEdit(), nullptr);
    EXPECT_NE(panel.enabledCheck(), nullptr);
    EXPECT_NE(panel.speedSpin(), nullptr);
    EXPECT_NE(panel.posXSpin(), nullptr);
    EXPECT_NE(panel.posYSpin(), nullptr);
    EXPECT_NE(panel.scaleXSpin(), nullptr);
    EXPECT_NE(panel.scaleYSpin(), nullptr);
    EXPECT_NE(panel.rotationSpin(), nullptr);
    EXPECT_NE(panel.opacitySpin(), nullptr);
}

TEST(PropertiesPanel, SizeHint)
{
    rt::PropertiesPanel panel;
    QSize hint = panel.sizeHint();
    EXPECT_GT(hint.width(), 0);
    EXPECT_GT(hint.height(), 0);
}

// ═══════════════════════════════════════════════════════════════════════════
//  PropertiesPanel — SpineClip binding
// ═══════════════════════════════════════════════════════════════════════════

TEST(PropertiesPanel, BindSpineClip)
{
    rt::PropertiesPanel panel;
    rt::SpineClip clip;
    clip.setLabel("TestCharacter");
    clip.setCharacterName("Modernia");
    clip.setOutfit("outfit_01");
    clip.setStance(rt::CharacterStance::Aim);
    clip.setAnimationName("walk");
    clip.setLooping(false);
    clip.setTalking(true);
    clip.setAnimationSpeed(1.5f);
    clip.setSpeed(2.0);
    clip.setEnabled(false);

    panel.setClip(&clip);

    // Identity
    EXPECT_EQ(panel.labelEdit()->text().toStdString(), "TestCharacter");
    EXPECT_FALSE(panel.enabledCheck()->isChecked());
    EXPECT_DOUBLE_EQ(panel.speedSpin()->value(), 200.0);

    // Spine-specific
    EXPECT_EQ(panel.characterCombo()->currentText().toStdString(), "Modernia");
    EXPECT_EQ(panel.outfitCombo()->currentText().toStdString(), "outfit_01");
    EXPECT_EQ(panel.stanceCombo()->currentIndex(), 1); // Aim
    EXPECT_EQ(panel.animationCombo()->currentText().toStdString(), "walk");
    EXPECT_FALSE(panel.loopingCheck()->isChecked());
    EXPECT_TRUE(panel.talkingCheck()->isChecked());
    EXPECT_FLOAT_EQ(static_cast<float>(panel.animSpeedSpin()->value()), 1.5f);
}

TEST(PropertiesPanel, SpinePropertyChanges)
{
    rt::PropertiesPanel panel;
    rt::SpineClip clip;
    clip.setCharacterName("Crown");

    panel.setClip(&clip);
    QSignalSpy spy(&panel, &rt::PropertiesPanel::propertyChanged);

    // Change character name via the combo box
    panel.characterCombo()->addItem("Dorothy");
    panel.characterCombo()->setCurrentText("Dorothy");
    EXPECT_EQ(clip.characterName(), "Dorothy");
    EXPECT_GE(spy.count(), 1);

    // Change animation
    panel.animationCombo()->addItem("run");
    panel.animationCombo()->setCurrentText("run");
    EXPECT_EQ(clip.animationName(), "run");

    // Change stance
    panel.stanceCombo()->setCurrentIndex(2); // Cover
    EXPECT_EQ(clip.stance(), rt::CharacterStance::Cover);

    // Change looping
    panel.loopingCheck()->setChecked(true);
    EXPECT_TRUE(clip.isLooping());

    // Change talking
    panel.talkingCheck()->setChecked(false);
    EXPECT_FALSE(clip.isTalking());
}

// ═══════════════════════════════════════════════════════════════════════════
//  PropertiesPanel — VideoClip binding
// ═══════════════════════════════════════════════════════════════════════════

TEST(PropertiesPanel, BindVideoClip)
{
    rt::PropertiesPanel panel;
    rt::VideoClip clip;
    clip.setLabel("MyVideo");
    clip.setMediaPath("/path/to/video.mp4");
    clip.setVolume(0.75f);
    clip.setSpeed(1.5);

    panel.setClip(&clip);

    EXPECT_EQ(panel.labelEdit()->text().toStdString(), "MyVideo");
    EXPECT_DOUBLE_EQ(panel.speedSpin()->value(), 150.0);
    EXPECT_EQ(panel.mediaPathLabel()->text().toStdString(), "/path/to/video.mp4");
    EXPECT_FLOAT_EQ(static_cast<float>(panel.volumeSpin()->value()), 0.75f);
}

TEST(PropertiesPanel, VideoVolumeChange)
{
    rt::PropertiesPanel panel;
    rt::VideoClip clip;
    clip.setVolume(1.0f);

    panel.setClip(&clip);
    QSignalSpy spy(&panel, &rt::PropertiesPanel::propertyChanged);

    panel.volumeSpin()->setValue(0.5);
    panel.volumeSpin()->editingFinished();

    EXPECT_FLOAT_EQ(clip.volume(), 0.5f);
    EXPECT_GE(spy.count(), 1);
}

// ═══════════════════════════════════════════════════════════════════════════
//  PropertiesPanel — AudioClip binding
// ═══════════════════════════════════════════════════════════════════════════

TEST(PropertiesPanel, BindAudioClip)
{
    rt::PropertiesPanel panel;
    rt::AudioClip clip;
    clip.setLabel("BGM");
    clip.setFadeInDuration(4800);
    clip.setFadeOutDuration(9600);

    panel.setClip(&clip);

    EXPECT_EQ(panel.labelEdit()->text().toStdString(), "BGM");
    EXPECT_NE(panel.audioVolumeSpin(), nullptr);
    EXPECT_NE(panel.panSpin(), nullptr);
    EXPECT_DOUBLE_EQ(panel.fadeInSpin()->value(), 4800.0);
    EXPECT_DOUBLE_EQ(panel.fadeOutSpin()->value(), 9600.0);
}

TEST(PropertiesPanel, AudioFadeChanges)
{
    rt::PropertiesPanel panel;
    rt::AudioClip clip;
    clip.setFadeInDuration(0);
    clip.setFadeOutDuration(0);

    panel.setClip(&clip);

    panel.fadeInSpin()->setValue(2400);
    panel.fadeInSpin()->editingFinished();
    EXPECT_EQ(clip.fadeInDuration(), 2400);

    panel.fadeOutSpin()->setValue(4800);
    panel.fadeOutSpin()->editingFinished();
    EXPECT_EQ(clip.fadeOutDuration(), 4800);
}

TEST(TimelinePanel, PasteAudioAttributesCopiesTracksAndUndoRestoresThem)
{
    rt::Timeline timeline;
    rt::Track* audioTrack = timeline.addAudioTrack("A1");

    auto source = std::make_unique<rt::AudioClip>();
    source->setTimelineIn(0);
    source->setDuration(48000);
    source->volume().addKeyframe(0, 0.25f);
    source->volume().addKeyframe(24000, 0.75f);
    source->pan().setDefaultValue(-0.4f);
    rt::AudioClip* sourcePtr = source.get();
    const uint64_t sourceId = source->id();
    ASSERT_NE(audioTrack->addClip(std::move(source)), nullptr);

    auto target = std::make_unique<rt::AudioClip>();
    target->setTimelineIn(48000);
    target->setDuration(48000);
    target->volume().addKeyframe(0, 0.9f);
    target->volume().addKeyframe(12000, 0.7f);
    target->pan().setDefaultValue(0.2f);
    rt::AudioClip* targetPtr = target.get();
    const uint64_t targetId = target->id();
    ASSERT_NE(audioTrack->addClip(std::move(target)), nullptr);

    rt::CommandStack stack;
    rt::TimelinePanel panel;
    panel.setCommandStack(&stack);
    panel.setTimeline(&timeline);

    size_t audioTrackIndex = SIZE_MAX;
    for (size_t i = 0; i < timeline.trackCount(); ++i) {
        if (timeline.track(i)->findClipIndexById(sourceId)
            < timeline.track(i)->clipCount()) {
            audioTrackIndex = i;
            break;
        }
    }
    ASSERT_NE(audioTrackIndex, SIZE_MAX);

    panel.selection().selectClip({audioTrackIndex, sourceId});
    panel.copyAttributesFromSelection();
    panel.selection().selectClip({audioTrackIndex, targetId});

    bool acceptedPasteDialog = false;
    QTimer::singleShot(0, [&acceptedPasteDialog]() {
        if (auto* dialog = qobject_cast<QDialog*>(
                QApplication::activeModalWidget())) {
            acceptedPasteDialog = true;
            dialog->accept();
        }
    });
    panel.showPasteAttributesDialog();
    ASSERT_TRUE(acceptedPasteDialog);

    ASSERT_EQ(targetPtr->volume().keyframeCount(), 2u);
    EXPECT_FLOAT_EQ(targetPtr->volume().keyframe(0).value, 0.25f);
    EXPECT_FLOAT_EQ(targetPtr->volume().keyframe(1).value, 0.75f);
    EXPECT_EQ(targetPtr->pan().keyframeCount(), 0u);
    EXPECT_FLOAT_EQ(targetPtr->pan().defaultValue(), -0.4f);

    // The inspector reads the pasted model value shown to the user.
    rt::PropertiesPanel inspector;
    inspector.setClip(targetPtr);
    EXPECT_FLOAT_EQ(static_cast<float>(inspector.audioVolumeSpin()->value()),
                    0.25f);

    // A later copy must not change what redo applies for this command.
    sourcePtr->volume() = rt::KeyframeTrack<float>(0.1f);
    panel.selection().selectClip({audioTrackIndex, sourceId});
    panel.copyAttributesFromSelection();

    ASSERT_TRUE(stack.undo());
    ASSERT_EQ(targetPtr->volume().keyframeCount(), 2u);
    EXPECT_FLOAT_EQ(targetPtr->volume().keyframe(0).value, 0.9f);
    EXPECT_FLOAT_EQ(targetPtr->volume().keyframe(1).value, 0.7f);
    EXPECT_FLOAT_EQ(targetPtr->pan().defaultValue(), 0.2f);

    ASSERT_TRUE(stack.redo());
    ASSERT_EQ(targetPtr->volume().keyframeCount(), 2u);
    EXPECT_FLOAT_EQ(targetPtr->volume().keyframe(0).value, 0.25f);
    EXPECT_FLOAT_EQ(targetPtr->volume().keyframe(1).value, 0.75f);
    EXPECT_FLOAT_EQ(targetPtr->pan().defaultValue(), -0.4f);
}

// ═══════════════════════════════════════════════════════════════════════════
//  PropertiesPanel — TitleClip binding
// ═══════════════════════════════════════════════════════════════════════════

TEST(PropertiesPanel, BindTitleClip)
{
    rt::PropertiesPanel panel;
    rt::TitleClip clip;
    clip.setLabel("Title Card");
    clip.setText("Hello World");
    clip.setFontFamily("Helvetica");
    clip.setFontSize(48.0f);
    clip.setBold(true);
    clip.setItalic(false);
    clip.setAlignment(rt::TextAlign::Right);

    panel.setClip(&clip);

    EXPECT_EQ(panel.labelEdit()->text().toStdString(), "Title Card");
    EXPECT_EQ(panel.textEdit()->text().toStdString(), "Hello World");
    EXPECT_EQ(panel.fontFamilyEdit()->text().toStdString(), "Helvetica");
    EXPECT_FLOAT_EQ(static_cast<float>(panel.fontSizeSpin()->value()), 48.0f);
    EXPECT_TRUE(panel.boldCheck()->isChecked());
    EXPECT_FALSE(panel.italicCheck()->isChecked());
    EXPECT_EQ(panel.alignCombo()->currentIndex(), 2); // Right
}

TEST(PropertiesPanel, TitleTextChange)
{
    rt::PropertiesPanel panel;
    rt::TitleClip clip;
    clip.setText("Old Text");

    panel.setClip(&clip);
    QSignalSpy spy(&panel, &rt::PropertiesPanel::propertyChanged);

    panel.textEdit()->setText("New Text");
    panel.textEdit()->editingFinished();

    EXPECT_EQ(clip.text(), "New Text");
    EXPECT_GE(spy.count(), 1);
}

TEST(PropertiesPanel, TitleFontChange)
{
    rt::PropertiesPanel panel;
    rt::TitleClip clip;

    panel.setClip(&clip);

    panel.fontFamilyEdit()->setText("Comic Sans MS");
    panel.fontFamilyEdit()->editingFinished();
    EXPECT_EQ(clip.fontFamily(), "Comic Sans MS");

    panel.fontSizeSpin()->setValue(24.0);
    panel.fontSizeSpin()->editingFinished();
    EXPECT_FLOAT_EQ(clip.fontSize(), 24.0f);
}

TEST(PropertiesPanel, TitleBoldItalicChange)
{
    rt::PropertiesPanel panel;
    rt::TitleClip clip;
    clip.setBold(false);
    clip.setItalic(false);

    panel.setClip(&clip);

    panel.boldCheck()->setChecked(true);
    EXPECT_TRUE(clip.isBold());

    panel.italicCheck()->setChecked(true);
    EXPECT_TRUE(clip.isItalic());
}

TEST(PropertiesPanel, TitleAlignChange)
{
    rt::PropertiesPanel panel;
    rt::TitleClip clip;
    clip.setAlignment(rt::TextAlign::Left);

    panel.setClip(&clip);

    panel.alignCombo()->setCurrentIndex(1); // Center
    EXPECT_EQ(clip.alignment(), rt::TextAlign::Center);

    panel.alignCombo()->setCurrentIndex(2); // Right
    EXPECT_EQ(clip.alignment(), rt::TextAlign::Right);
}

// ═══════════════════════════════════════════════════════════════════════════
//  PropertiesPanel — common property changes
// ═══════════════════════════════════════════════════════════════════════════

TEST(PropertiesPanel, LabelChange)
{
    rt::PropertiesPanel panel;
    rt::SpineClip clip;
    clip.setLabel("Original");

    panel.setClip(&clip);
    QSignalSpy spy(&panel, &rt::PropertiesPanel::propertyChanged);

    panel.labelEdit()->setText("Renamed");
    panel.labelEdit()->editingFinished();

    EXPECT_EQ(clip.label(), "Renamed");
    EXPECT_GE(spy.count(), 1);
}

TEST(PropertiesPanel, EnabledChange)
{
    rt::PropertiesPanel panel;
    rt::SpineClip clip;
    clip.setEnabled(true);

    panel.setClip(&clip);

    panel.enabledCheck()->setChecked(false);
    EXPECT_FALSE(clip.isEnabled());

    panel.enabledCheck()->setChecked(true);
    EXPECT_TRUE(clip.isEnabled());
}

TEST(PropertiesPanel, SpeedChange)
{
    rt::PropertiesPanel panel;
    rt::VideoClip clip;
    clip.setSpeed(1.0);

    panel.setClip(&clip);

    panel.speedSpin()->setValue(250.0);
    panel.speedSpin()->editingFinished();

    EXPECT_DOUBLE_EQ(clip.speed(), 2.5);
}

TEST(PropertiesPanel, TransformChange)
{
    rt::PropertiesPanel panel;
    rt::VideoClip clip;

    panel.setClip(&clip);

    auto scrubCommit = [](rt::ScrubbySpinBox* spin,
                          double oldValue, double newValue) {
        spin->setValue(newValue);
        emit spin->valueScrubbed(newValue);
        emit spin->valueCommitted(oldValue, newValue);
    };
    scrubCommit(panel.posXSpin(), 0.0, 100.0);
    scrubCommit(panel.posYSpin(), 0.0, 200.0);
    scrubCommit(panel.scaleXSpin(), 100.0, 150.0); // internal 1.5
    scrubCommit(panel.scaleYSpin(), 100.0, 80.0);  // internal 0.8
    scrubCommit(panel.rotationSpin(), 0.0, 45.0);
    scrubCommit(panel.opacitySpin(), 100.0, 70.0); // internal 0.7

    EXPECT_FLOAT_EQ(clip.positionX().evaluate(0), 100.0f);
    EXPECT_FLOAT_EQ(clip.positionY().evaluate(0), 200.0f);
    EXPECT_FLOAT_EQ(clip.scaleX().evaluate(0), 1.5f);
    EXPECT_FLOAT_EQ(clip.scaleY().evaluate(0), 0.8f);
    EXPECT_FLOAT_EQ(clip.rotation().evaluate(0), 45.0f);
    EXPECT_FLOAT_EQ(clip.opacity().evaluate(0), 0.7f);
}

// Repro: flip horizontal, then change scale; one undo must revert ONLY the
// scale change, leaving the flip intact (separate undo steps).
TEST(PropertiesPanel, FlipThenScaleUndoIsSeparate)
{
    rt::CommandStack stack;
    rt::PropertiesPanel panel;
    rt::VideoClip clip;

    panel.setCommandStack(&stack);
    panel.setClip(&clip);

    // 1) Flip horizontal via the checkbox.
    panel.flipHCheck()->setChecked(true);
    EXPECT_LT(clip.scaleX().evaluate(0), 0.0f) << "flip should make scaleX negative";
    EXPECT_EQ(stack.undoCount(), 1u) << "flip should be one undo command";

    // 2) Change the scale magnitude. The spinbox now shows the flipped
    //    (negative) value; user scrubs it to a larger magnitude.
    const double flippedUi = panel.scaleXSpin()->value(); // ~ -100
    panel.scaleXSpin()->setValue(flippedUi * 2.0);         // ~ -200
    emit panel.scaleXSpin()->valueScrubbed(panel.scaleXSpin()->value());     // live write
    emit panel.scaleXSpin()->valueCommitted(flippedUi, panel.scaleXSpin()->value());

    EXPECT_EQ(stack.undoCount(), 2u) << "flip + scale = two undo commands";
    EXPECT_FLOAT_EQ(clip.scaleX().evaluate(0), -2.0f);

    // 3) One undo: scale reverts, flip stays.
    stack.undo();
    EXPECT_LT(clip.scaleX().evaluate(0), 0.0f) << "flip must survive the scale undo";
    EXPECT_FLOAT_EQ(clip.scaleX().evaluate(0), -1.0f);

    // 4) Second undo: flip reverts.
    stack.undo();
    EXPECT_FLOAT_EQ(clip.scaleX().evaluate(0), 1.0f);
}

TEST(EffectControlsPanel, BlendModeChangeIsUndoable)
{
    rt::VideoClip clip;
    rt::CommandStack stack;
    rt::EffectControlsPanel panel;
    panel.setCommandStack(&stack);
    panel.setClip(&clip);

    auto* blendMode = panel.findChild<QComboBox*>(QStringLiteral("blendModeCombo"));
    ASSERT_NE(blendMode, nullptr);
    EXPECT_EQ(blendMode->currentIndex(), 0);

    blendMode->setCurrentIndex(2); // Screen
    EXPECT_EQ(clip.blendMode(), 2);
    ASSERT_EQ(stack.undoCount(), 1u);
    EXPECT_EQ(stack.undoDescription(), "Blend Mode");

    ASSERT_TRUE(stack.undo());
    EXPECT_EQ(clip.blendMode(), 0);
    EXPECT_EQ(blendMode->currentIndex(), 0);

    ASSERT_TRUE(stack.redo());
    EXPECT_EQ(clip.blendMode(), 2);
    EXPECT_EQ(blendMode->currentIndex(), 2);
}

TEST(EffectControlsPanel, ShutterAngleIsKeyframeableAndUndoable)
{
    rt::VideoClip clip;
    rt::CommandStack stack;
    rt::EffectControlsPanel panel;
    panel.setCommandStack(&stack);
    panel.setClip(&clip);

    auto* shutter = panel.findChild<rt::ScrubbySpinBox*>(
        QStringLiteral("shutterAngleSpin"));
    ASSERT_NE(shutter, nullptr);
    EXPECT_DOUBLE_EQ(shutter->value(), 0.0);

    panel.show();
    shutter->setFocus(Qt::OtherFocusReason);
    QApplication::processEvents();
    ASSERT_TRUE(shutter->hasFocus());
    shutter->setValue(180.0);
    emit shutter->editingFinished();
    EXPECT_FLOAT_EQ(clip.shutterAngle().evaluate(0), 180.0f);
    ASSERT_EQ(stack.undoCount(), 1u);

    ASSERT_TRUE(stack.undo());
    EXPECT_FLOAT_EQ(clip.shutterAngle().evaluate(0), 0.0f);
    ASSERT_TRUE(stack.redo());
    EXPECT_FLOAT_EQ(clip.shutterAngle().evaluate(0), 180.0f);
}

TEST(EffectControlsPanel, PositionDiamondUsesLivePlayheadInsteadOfPaintedTime)
{
    rt::VideoClip clip;
    clip.setTimelineIn(10000);
    clip.setDuration(48000);

    rt::Timeline timeline;
    timeline.setPlayheadPosition(16000);

    rt::EffectControlsPanel panel;
    panel.setTimeline(&timeline);
    panel.setClip(&clip);

    auto* positionRow = findEffectPropertyRow(
        panel, QStringLiteral("Position"));
    auto* diamond = findKeyframeDiamond(positionRow);
    ASSERT_NE(positionRow, nullptr);
    ASSERT_NE(diamond, nullptr);

    diamond->click();
    ASSERT_EQ(clip.positionX().keyframeCount(), 1u);
    ASSERT_EQ(clip.positionY().keyframeCount(), 1u);
    EXPECT_EQ(clip.positionX().keyframe(0).time, 6000);
    EXPECT_EQ(clip.positionY().keyframe(0).time, 6000);
    EXPECT_TRUE(positionRow->stopwatchButton()->isChecked());

    // Deliberately advance only the model playhead. This is the state seen
    // between throttled Effect Controls paints during playback. The old
    // click handler still believed it was at 6000 and deleted the first key.
    timeline.setPlayheadPosition(34000);
    diamond->click();

    ASSERT_EQ(clip.positionX().keyframeCount(), 2u);
    ASSERT_EQ(clip.positionY().keyframeCount(), 2u);
    EXPECT_EQ(clip.positionX().keyframe(0).time, 6000);
    EXPECT_EQ(clip.positionY().keyframe(0).time, 6000);
    EXPECT_EQ(clip.positionX().keyframe(1).time, 24000);
    EXPECT_EQ(clip.positionY().keyframe(1).time, 24000);
}

TEST(EffectControlsPanel, PositionDiamondUndoRedoIsOneAtomicAction)
{
    rt::VideoClip clip;
    clip.setDuration(48000);
    rt::CommandStack stack;
    rt::EffectControlsPanel panel;
    panel.setCommandStack(&stack);
    panel.setClip(&clip);
    panel.setPlayheadTick(12000);

    auto* row = findEffectPropertyRow(panel, QStringLiteral("Position"));
    auto* diamond = findKeyframeDiamond(row);
    ASSERT_NE(diamond, nullptr);
    diamond->click();

    ASSERT_EQ(stack.undoCount(), 1u);
    ASSERT_EQ(clip.positionX().keyframeCount(), 1u);
    ASSERT_EQ(clip.positionY().keyframeCount(), 1u);
    ASSERT_TRUE(stack.undo());
    EXPECT_EQ(clip.positionX().keyframeCount(), 0u);
    EXPECT_EQ(clip.positionY().keyframeCount(), 0u);
    ASSERT_TRUE(stack.redo());
    EXPECT_EQ(clip.positionX().keyframeCount(), 1u);
    EXPECT_EQ(clip.positionY().keyframeCount(), 1u);

    // Deleting the compound diamond is one action as well.
    diamond->click();
    ASSERT_EQ(stack.undoCount(), 2u);
    EXPECT_EQ(clip.positionX().keyframeCount(), 0u);
    EXPECT_EQ(clip.positionY().keyframeCount(), 0u);
    ASSERT_TRUE(stack.undo());
    EXPECT_EQ(clip.positionX().keyframeCount(), 1u);
    EXPECT_EQ(clip.positionY().keyframeCount(), 1u);
}

TEST(EffectControlsPanel, UniformScaleDiamondUndoRedoIsOneAtomicAction)
{
    rt::VideoClip clip;
    clip.setDuration(48000);
    rt::CommandStack stack;
    rt::EffectControlsPanel panel;
    panel.setCommandStack(&stack);
    panel.setClip(&clip);
    panel.setPlayheadTick(18000);

    auto* row = findEffectPropertyRow(panel, QStringLiteral("Scale"));
    auto* diamond = findKeyframeDiamond(row);
    ASSERT_NE(diamond, nullptr);
    diamond->click();

    ASSERT_EQ(stack.undoCount(), 1u);
    ASSERT_EQ(clip.scaleX().keyframeCount(), 1u);
    ASSERT_EQ(clip.scaleY().keyframeCount(), 1u);
    ASSERT_TRUE(stack.undo());
    EXPECT_EQ(clip.scaleX().keyframeCount(), 0u);
    EXPECT_EQ(clip.scaleY().keyframeCount(), 0u);
    ASSERT_TRUE(stack.redo());
    EXPECT_EQ(clip.scaleX().keyframeCount(), 1u);
    EXPECT_EQ(clip.scaleY().keyframeCount(), 1u);
}

TEST(EffectControlsPanel, PositionStopwatchSeedsBothAxes)
{
    rt::VideoClip clip;
    clip.setDuration(48000);

    rt::CommandStack stack;
    rt::EffectControlsPanel panel;
    panel.setCommandStack(&stack);
    panel.setClip(&clip);
    panel.setPlayheadTick(12000);

    auto* positionRow = findEffectPropertyRow(
        panel, QStringLiteral("Position"));
    ASSERT_NE(positionRow, nullptr);
    ASSERT_NE(positionRow->stopwatchButton(), nullptr);

    positionRow->stopwatchButton()->click();

    ASSERT_EQ(clip.positionX().keyframeCount(), 1u);
    ASSERT_EQ(clip.positionY().keyframeCount(), 1u);
    EXPECT_EQ(clip.positionX().keyframe(0).time, 12000);
    EXPECT_EQ(clip.positionY().keyframe(0).time, 12000);
    ASSERT_EQ(stack.undoCount(), 1u);
    ASSERT_TRUE(stack.undo());
    EXPECT_EQ(clip.positionX().keyframeCount(), 0u);
    EXPECT_EQ(clip.positionY().keyframeCount(), 0u);
    ASSERT_TRUE(stack.redo());
    EXPECT_EQ(clip.positionX().keyframeCount(), 1u);
    EXPECT_EQ(clip.positionY().keyframeCount(), 1u);
}

TEST(EffectControlsPanel, PositionValueUndoRemovesAutoCreatedSiblingKey)
{
    rt::VideoClip clip;
    clip.setDuration(48000);
    clip.positionX().addKeyframe(0, 0.0f);
    clip.positionX().addKeyframe(48000, 480.0f);
    clip.positionY().addKeyframe(0, 10.0f);
    clip.positionY().addKeyframe(48000, 20.0f);

    rt::CommandStack stack;
    rt::EffectControlsPanel panel;
    panel.setCommandStack(&stack);
    panel.setClip(&clip);
    panel.setPlayheadTick(24000);

    auto* row = findEffectPropertyRow(panel, QStringLiteral("Position"));
    ASSERT_NE(row, nullptr);
    const auto spins = row->findChildren<rt::ScrubbySpinBox*>();
    ASSERT_GE(spins.size(), 2);
    auto* xSpin = spins[0];
    const double oldValue = xSpin->value();
    xSpin->setValue(300.0);
    emit xSpin->valueScrubbed(300.0);
    emit xSpin->valueCommitted(oldValue, 300.0);

    ASSERT_EQ(stack.undoCount(), 1u);
    ASSERT_EQ(clip.positionX().keyframeCount(), 3u);
    ASSERT_EQ(clip.positionY().keyframeCount(), 3u);
    EXPECT_TRUE(clip.positionX().hasKeyframeAt(24000));
    EXPECT_TRUE(clip.positionY().hasKeyframeAt(24000));

    ASSERT_TRUE(stack.undo());
    EXPECT_EQ(clip.positionX().keyframeCount(), 2u);
    EXPECT_EQ(clip.positionY().keyframeCount(), 2u);
    EXPECT_FALSE(clip.positionX().hasKeyframeAt(24000));
    EXPECT_FALSE(clip.positionY().hasKeyframeAt(24000));

    ASSERT_TRUE(stack.redo());
    EXPECT_EQ(clip.positionX().keyframeCount(), 3u);
    EXPECT_EQ(clip.positionY().keyframeCount(), 3u);
    EXPECT_TRUE(clip.positionX().hasKeyframeAt(24000));
    EXPECT_TRUE(clip.positionY().hasKeyframeAt(24000));
}

TEST(EffectControlsPanel, GraphicLayerPositionDiamondRetargetsBothAxes)
{
    rt::GraphicClip clip;
    clip.setDuration(48000);
    auto* layer = clip.addTextLayer("Layer");
    ASSERT_NE(layer, nullptr);

    rt::EffectControlsPanel panel;
    panel.setClip(&clip);
    panel.setSelectedGraphicLayer(layer);
    panel.setPlayheadTick(12000);

    auto* positionRow = findEffectPropertyRow(
        panel, QStringLiteral("Position"));
    auto* diamond = findKeyframeDiamond(positionRow);
    ASSERT_NE(positionRow, nullptr);
    ASSERT_NE(diamond, nullptr);

    diamond->click();

    ASSERT_EQ(layer->transform().posX.keyframeCount(), 1u);
    ASSERT_EQ(layer->transform().posY.keyframeCount(), 1u);
    EXPECT_EQ(layer->transform().posX.keyframe(0).time, 12000);
    EXPECT_EQ(layer->transform().posY.keyframe(0).time, 12000);
    EXPECT_EQ(clip.positionX().keyframeCount(), 0u);
    EXPECT_EQ(clip.positionY().keyframeCount(), 0u);
}

TEST(KeyframeEditor, BatchDeleteAndInterpolationUseSingleUndoStep)
{
    rt::VideoClip clip;
    clip.positionX().addKeyframe(10000, 10.0f);
    clip.positionY().addKeyframe(10000, 20.0f);
    rt::CommandStack stack;
    rt::KeyframeEditor editor;
    editor.setCommandStack(&stack);
    editor.setClip(&clip);

    editor.selectKey(1, 0);
    editor.selectKey(2, 0, true);
    editor.setInterpolation(static_cast<int>(rt::InterpMode::Bezier));
    ASSERT_EQ(stack.undoCount(), 1u);
    EXPECT_EQ(clip.positionX().keyframe(0).interp, rt::InterpMode::Bezier);
    EXPECT_EQ(clip.positionY().keyframe(0).interp, rt::InterpMode::Bezier);
    ASSERT_TRUE(stack.undo());
    EXPECT_EQ(clip.positionX().keyframe(0).interp, rt::InterpMode::Linear);
    EXPECT_EQ(clip.positionY().keyframe(0).interp, rt::InterpMode::Linear);
    ASSERT_TRUE(stack.redo());
    EXPECT_EQ(clip.positionX().keyframe(0).interp, rt::InterpMode::Bezier);
    EXPECT_EQ(clip.positionY().keyframe(0).interp, rt::InterpMode::Bezier);

    editor.deleteSelectedKeyframes();
    ASSERT_EQ(stack.undoCount(), 2u);
    EXPECT_EQ(clip.positionX().keyframeCount(), 0u);
    EXPECT_EQ(clip.positionY().keyframeCount(), 0u);
    ASSERT_TRUE(stack.undo());
    EXPECT_EQ(clip.positionX().keyframeCount(), 1u);
    EXPECT_EQ(clip.positionY().keyframeCount(), 1u);
    ASSERT_TRUE(stack.redo());
    EXPECT_EQ(clip.positionX().keyframeCount(), 0u);
    EXPECT_EQ(clip.positionY().keyframeCount(), 0u);
}

TEST(KeyframeEditor, BatchPasteIsAtomicAndPreservesHandles)
{
    rt::VideoClip clip;
    clip.positionX().addKeyframe(10000, 10.0f, rt::InterpMode::Bezier);
    clip.positionY().addKeyframe(10000, 20.0f, rt::InterpMode::Bezier);
    clip.positionX().keyframe(0).bezierOutX = 0.21f;
    clip.positionX().keyframe(0).bezierOutY = 0.37f;
    clip.positionY().keyframe(0).spatialOutX = 14.0f;
    clip.positionY().keyframe(0).spatialOutY = -8.0f;

    rt::CommandStack stack;
    rt::KeyframeEditor editor;
    editor.setCommandStack(&stack);
    editor.setClip(&clip);
    editor.selectKey(1, 0);
    editor.selectKey(2, 0, true);
    editor.copySelectedKeyframes();
    editor.pasteKeyframes(30000);

    ASSERT_EQ(stack.undoCount(), 1u);
    ASSERT_EQ(clip.positionX().keyframeCount(), 2u);
    ASSERT_EQ(clip.positionY().keyframeCount(), 2u);
    EXPECT_FLOAT_EQ(clip.positionX().keyframe(1).bezierOutX, 0.21f);
    EXPECT_FLOAT_EQ(clip.positionX().keyframe(1).bezierOutY, 0.37f);
    EXPECT_FLOAT_EQ(clip.positionY().keyframe(1).spatialOutX, 14.0f);
    EXPECT_FLOAT_EQ(clip.positionY().keyframe(1).spatialOutY, -8.0f);

    ASSERT_TRUE(stack.undo());
    EXPECT_EQ(clip.positionX().keyframeCount(), 1u);
    EXPECT_EQ(clip.positionY().keyframeCount(), 1u);
    ASSERT_TRUE(stack.redo());
    ASSERT_EQ(clip.positionX().keyframeCount(), 2u);
    ASSERT_EQ(clip.positionY().keyframeCount(), 2u);
    EXPECT_FLOAT_EQ(clip.positionX().keyframe(1).bezierOutX, 0.21f);
    EXPECT_FLOAT_EQ(clip.positionY().keyframe(1).spatialOutX, 14.0f);
}

TEST(KeyframeEditor, TangentDragUndoRedoRestoresExactHandle)
{
    rt::VideoClip clip;
    clip.positionX().addKeyframe(0, 0.0f, rt::InterpMode::Bezier);
    clip.positionX().addKeyframe(100, 100.0f, rt::InterpMode::Bezier);

    rt::CommandStack stack;
    rt::KeyframeEditor editor;
    editor.setCommandStack(&stack);
    editor.setClip(&clip);
    editor.resize(800, 400);
    editor.setViewRange(0.0, 100.0, 0.0, 100.0);
    editor.selectKey(1, 0);
    editor.show();
    QApplication::processEvents();

    const float oldX = clip.positionX().keyframe(0).bezierOutX;
    const float oldY = clip.positionX().keyframe(0).bezierOutY;
    const QPoint start = editor.graphToPixel(oldX * 100.0, oldY * 100.0)
                             .toPoint();
    const QPoint finish = editor.graphToPixel(40.0, 30.0).toPoint();
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(&editor, finish);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, finish);

    ASSERT_EQ(stack.undoCount(), 1u);
    const float changedX = clip.positionX().keyframe(0).bezierOutX;
    const float changedY = clip.positionX().keyframe(0).bezierOutY;
    EXPECT_NE(changedX, oldX);
    EXPECT_NE(changedY, oldY);
    ASSERT_TRUE(stack.undo());
    EXPECT_FLOAT_EQ(clip.positionX().keyframe(0).bezierOutX, oldX);
    EXPECT_FLOAT_EQ(clip.positionX().keyframe(0).bezierOutY, oldY);
    ASSERT_TRUE(stack.redo());
    EXPECT_FLOAT_EQ(clip.positionX().keyframe(0).bezierOutX, changedX);
    EXPECT_FLOAT_EQ(clip.positionX().keyframe(0).bezierOutY, changedY);
}

TEST(EffectControlsPanel, TintUsesPremiereStyleColorAndAmountControls)
{
    rt::VideoClip clip;
    clip.effects().addEffect(std::make_unique<rt::Tint>());

    rt::CommandStack stack;
    rt::EffectControlsPanel panel;
    panel.setCommandStack(&stack);
    panel.setClip(&clip);

    EXPECT_NE(panel.findChild<QPushButton*>(
                  QStringLiteral("tintMapBlackButton")), nullptr);
    EXPECT_NE(panel.findChild<QPushButton*>(
                  QStringLiteral("tintMapWhiteButton")), nullptr);

    auto* amount = panel.findChild<rt::ScrubbySpinBox*>(
        QStringLiteral("tintAmountSpin"));
    ASSERT_NE(amount, nullptr);
    EXPECT_DOUBLE_EQ(amount->value(), 100.0);

    amount->setValue(35.0);
    emit amount->valueScrubbed(35.0);
    emit amount->valueCommitted(100.0, 35.0);

    auto& tint = clip.effects().effect(0);
    EXPECT_FLOAT_EQ(tint.evalParam(rt::Tint::AmountToTint, 0), 35.0f);
    ASSERT_EQ(stack.undoCount(), 1u);
    ASSERT_TRUE(stack.undo());
    EXPECT_FLOAT_EQ(tint.evalParam(rt::Tint::AmountToTint, 0), 100.0f);
}

TEST(EffectControlsPanel, PenMaskButtonArmsToolWithoutCreatingPlaceholder)
{
    rt::VideoClip clip;
    rt::EffectControlsPanel panel;
    panel.setClip(&clip);

    QSignalSpy spy(&panel, &rt::EffectControlsPanel::penMaskToolRequested);
    auto* penButton = panel.findChild<QToolButton*>(
        QStringLiteral("clipPenMaskButton"));
    ASSERT_NE(penButton, nullptr);

    penButton->click();

    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(0).toULongLong(), 0u);
    EXPECT_TRUE(clip.masks().empty());
}

TEST(EffectControlsPanel, InPlaceCreatedPenMaskAppearsAndCanBeSelected)
{
    rt::VideoClip clip;
    rt::EffectControlsPanel panel;
    panel.resize(900, 600);
    panel.setClip(&clip);

    rt::OpacityMask mask;
    mask.shape = rt::MaskShape::FreeDrawBezier;
    mask.name = "Mask 1";
    mask.base.vertices = {
        {0.2f, 0.2f, 0.0f, 0.0f, 0.0f, 0.0f},
        {0.8f, 0.2f, 0.0f, 0.0f, 0.0f, 0.0f},
        {0.5f, 0.8f, 0.0f, 0.0f, 0.0f, 0.0f},
    };
    clip.addMask(mask); // same in-place mutation used by the Program Monitor
    const uint64_t maskId = clip.masks()[0].maskId;

    panel.refresh();
    QSignalSpy selectedSpy(&panel, &rt::EffectControlsPanel::maskSelected);
    EXPECT_TRUE(panel.selectMaskById(0, maskId));
    EXPECT_TRUE(panel.hasSelectedMask());
    EXPECT_NE(panel.findChild<QWidget*>(QStringLiteral("maskHeader")), nullptr);
    ASSERT_EQ(selectedSpy.count(), 1);
    EXPECT_EQ(selectedSpy.at(0).at(0).toInt(), 0);
    EXPECT_EQ(selectedSpy.at(0).at(1).toULongLong(), 0u);
}

TEST(EffectControlsPanel, DeleteOnSelectedClipMaskHeaderIsUndoable)
{
    rt::VideoClip clip;
    rt::OpacityMask mask;
    mask.name = "Clip Mask";
    clip.addMask(mask);
    const uint64_t maskId = clip.masks()[0].maskId;

    rt::CommandStack stack;
    DeleteProbeWidget workspace;
    rt::EffectControlsPanel panel(&workspace);
    panel.setCommandStack(&stack);
    panel.resize(900, 600);
    panel.setClip(&clip);
    workspace.resize(900, 600);
    workspace.show();
    panel.show();
    QApplication::processEvents();

    QWidget* header = nullptr;
    for (auto* candidate : panel.findChildren<QWidget*>(
             QStringLiteral("maskHeader"))) {
        if (candidate->property("maskEffectId").toULongLong() == 0
            && candidate->property("maskId").toULongLong() == maskId) {
            header = candidate;
            break;
        }
    }
    ASSERT_NE(header, nullptr);

    auto* titleLabel = header->findChild<QLabel*>(
        QStringLiteral("maskTitleLabel"));
    ASSERT_NE(titleLabel, nullptr);
    const QPoint titleCenterGlobal =
        titleLabel->mapToGlobal(titleLabel->rect().center());
    QWidget* labelHitTarget = QApplication::widgetAt(titleCenterGlobal);
    ASSERT_EQ(labelHitTarget, header);
    QTest::mouseClick(labelHitTarget, Qt::LeftButton, Qt::NoModifier,
                      labelHitTarget->mapFromGlobal(titleCenterGlobal));
    EXPECT_TRUE(panel.hasFocus());
    EXPECT_TRUE(panel.hasSelectedMask());
    QTest::keyClick(&panel, Qt::Key_Delete);

    EXPECT_TRUE(clip.masks().empty());
    EXPECT_FALSE(panel.hasSelectedMask());
    EXPECT_EQ(workspace.deletePresses, 0)
        << "handled mask Delete must not reach workspace clip deletion";
    ASSERT_EQ(stack.undoCount(), 1u);
    EXPECT_EQ(stack.undoDescription(), "Delete Mask");
    ASSERT_TRUE(stack.undo());
    ASSERT_EQ(clip.masks().size(), 1u);
    EXPECT_EQ(clip.masks()[0].maskId, maskId);
    ASSERT_TRUE(stack.redo());
    EXPECT_TRUE(clip.masks().empty());
}

TEST(EffectControlsPanel, DeleteOnSelectedEffectMaskRowKeepsEffect)
{
    rt::VideoClip clip;
    auto blur = std::make_unique<rt::Blur>();
    rt::OpacityMask mask;
    mask.name = "Effect Mask";
    blur->addMask(mask);
    const uint64_t effectId = blur->id();
    const uint64_t maskId = blur->masks()[0].maskId;
    clip.effects().addEffect(std::move(blur));

    rt::CommandStack stack;
    rt::EffectControlsPanel panel;
    panel.setCommandStack(&stack);
    panel.resize(900, 600);
    panel.setClip(&clip);
    panel.show();
    QApplication::processEvents();

    QWidget* maskRow = nullptr;
    for (auto* candidate : panel.findChildren<QWidget*>(
             QStringLiteral("maskPathRow"))) {
        if (candidate->property("maskEffectId").toULongLong() == effectId
            && candidate->property("maskId").toULongLong() == maskId) {
            maskRow = candidate;
            break;
        }
    }
    ASSERT_NE(maskRow, nullptr);

    auto* pathLabel = maskRow->findChild<QLabel*>(
        QStringLiteral("maskPathLabel"));
    ASSERT_NE(pathLabel, nullptr);
    const QPoint pathCenterGlobal =
        pathLabel->mapToGlobal(pathLabel->rect().center());
    QWidget* labelHitTarget = QApplication::widgetAt(pathCenterGlobal);
    ASSERT_EQ(labelHitTarget, maskRow);
    QTest::mouseClick(labelHitTarget, Qt::LeftButton, Qt::NoModifier,
                      labelHitTarget->mapFromGlobal(pathCenterGlobal));
    EXPECT_TRUE(panel.hasFocus());
    QTest::keyClick(&panel, Qt::Key_Backspace);

    ASSERT_EQ(clip.effects().effectCount(), 1u)
        << "mask Delete must not bubble into effect/layer deletion";
    auto* effect = clip.effects().effectById(effectId);
    ASSERT_NE(effect, nullptr);
    EXPECT_TRUE(effect->masks().empty());
    EXPECT_EQ(stack.undoDescription(), "Delete Mask");

    ASSERT_TRUE(stack.undo());
    effect = clip.effects().effectById(effectId);
    ASSERT_NE(effect, nullptr);
    ASSERT_EQ(effect->masks().size(), 1u);
    EXPECT_EQ(effect->masks()[0].maskId, maskId);
}

TEST(EffectControlsPanel, MaskHeaderArrowClickSelectsBeforeItsAction)
{
    rt::VideoClip clip;
    rt::OpacityMask mask;
    mask.name = "Clip Mask";
    clip.addMask(mask);

    rt::CommandStack stack;
    DeleteProbeWidget workspace;
    rt::EffectControlsPanel panel(&workspace);
    panel.setCommandStack(&stack);
    panel.resize(900, 600);
    panel.setClip(&clip);
    workspace.resize(900, 600);
    workspace.show();
    panel.show();
    QApplication::processEvents();

    auto* header = panel.findChild<QWidget*>(QStringLiteral("maskHeader"));
    ASSERT_NE(header, nullptr);
    auto* arrow = header->findChild<QToolButton*>(
        QStringLiteral("maskCollapseButton"));
    ASSERT_NE(arrow, nullptr);
    QSignalSpy selectedSpy(&panel, &rt::EffectControlsPanel::maskSelected);
    const QString arrowBefore = arrow->text();

    QTest::mouseClick(arrow, Qt::LeftButton);

    EXPECT_NE(arrow->text(), arrowBefore)
        << "selection filtering must preserve the arrow's collapse action";
    ASSERT_GE(selectedSpy.count(), 1);
    QWidget* focused = QApplication::focusWidget();
    ASSERT_NE(focused, nullptr);
    ASSERT_TRUE(focused == &panel || panel.isAncestorOf(focused));
    QTest::keyClick(focused, Qt::Key_Delete);

    EXPECT_TRUE(clip.masks().empty());
    EXPECT_EQ(stack.undoDescription(), "Delete Mask");
    EXPECT_EQ(workspace.deletePresses, 0);
}

TEST(EffectControlsPanel, MaskPathStopwatchClickSelectsBeforeItsAction)
{
    rt::VideoClip clip;
    auto blur = std::make_unique<rt::Blur>();
    rt::OpacityMask mask;
    mask.name = "Effect Mask";
    blur->addMask(mask);
    const uint64_t effectId = blur->id();
    clip.effects().addEffect(std::move(blur));

    rt::CommandStack stack;
    DeleteProbeWidget workspace;
    rt::EffectControlsPanel panel(&workspace);
    panel.setCommandStack(&stack);
    panel.resize(900, 600);
    panel.setClip(&clip);
    workspace.resize(900, 600);
    workspace.show();
    panel.show();
    QApplication::processEvents();

    QToolButton* stopwatch = nullptr;
    for (auto* candidate : panel.findChildren<QToolButton*>(
             QStringLiteral("maskPathStopwatch"))) {
        auto* row = candidate->parentWidget();
        if (row && row->property("maskEffectId").toULongLong() == effectId) {
            stopwatch = candidate;
            break;
        }
    }
    ASSERT_NE(stopwatch, nullptr);
    QSignalSpy selectedSpy(&panel, &rt::EffectControlsPanel::maskSelected);

    QTest::mouseClick(stopwatch, Qt::LeftButton);

    auto* effect = clip.effects().effectById(effectId);
    ASSERT_NE(effect, nullptr);
    ASSERT_EQ(effect->masks().size(), 1u);
    EXPECT_TRUE(effect->masks()[0].pathAnimated)
        << "selection filtering must preserve the stopwatch toggle";
    ASSERT_GE(selectedSpy.count(), 1);
    QWidget* focused = QApplication::focusWidget();
    ASSERT_NE(focused, nullptr);
    ASSERT_TRUE(focused == &panel || panel.isAncestorOf(focused));
    QTest::keyClick(focused, Qt::Key_Delete);

    ASSERT_EQ(clip.effects().effectCount(), 1u);
    effect = clip.effects().effectById(effectId);
    ASSERT_NE(effect, nullptr);
    EXPECT_TRUE(effect->masks().empty());
    EXPECT_EQ(stack.undoDescription(), "Delete Mask");
    EXPECT_EQ(workspace.deletePresses, 0);
}

TEST(EffectControlsPanel, MaskPathStopwatchMatchesPropertyRowExactly)
{
    rt::VideoClip clip;
    rt::OpacityMask mask;
    mask.name = "Mask 1";
    clip.addMask(mask);

    rt::EffectControlsPanel panel;
    panel.resize(900, 600);
    panel.setClip(&clip);
    panel.show();
    QApplication::processEvents();

    auto* maskPathRow = panel.findChild<QWidget*>(
        QStringLiteral("maskPathRow"));
    auto* maskStopwatch = panel.findChild<QToolButton*>(
        QStringLiteral("maskPathStopwatch"));
    rt::PropertyRow* featherRow = nullptr;
    for (auto* row : panel.findChildren<rt::PropertyRow*>()) {
        if (row->propertyName() == QStringLiteral("Mask Feather")) {
            featherRow = row;
            break;
        }
    }
    ASSERT_NE(maskPathRow, nullptr);
    ASSERT_NE(maskStopwatch, nullptr);
    ASSERT_NE(featherRow, nullptr);
    auto* propertyStopwatch = featherRow->stopwatchButton();
    ASSERT_NE(propertyStopwatch, nullptr);

    EXPECT_EQ(maskStopwatch->size(), propertyStopwatch->size());
    EXPECT_EQ(maskStopwatch->size(), QSize(18, 18));
    EXPECT_EQ(maskStopwatch->iconSize(), propertyStopwatch->iconSize());
    EXPECT_EQ(maskStopwatch->iconSize(), QSize(14, 14));
    EXPECT_EQ(maskStopwatch->styleSheet(), propertyStopwatch->styleSheet());

    const QImage maskOff = maskStopwatch->icon().pixmap(
        maskStopwatch->iconSize(), QIcon::Normal, QIcon::Off).toImage();
    const QImage propertyOff = propertyStopwatch->icon().pixmap(
        propertyStopwatch->iconSize(), QIcon::Normal, QIcon::Off).toImage();
    const QImage maskOn = maskStopwatch->icon().pixmap(
        maskStopwatch->iconSize(), QIcon::Normal, QIcon::On).toImage();
    const QImage propertyOn = propertyStopwatch->icon().pixmap(
        propertyStopwatch->iconSize(), QIcon::Normal, QIcon::On).toImage();
    EXPECT_TRUE(maskOff == propertyOff);
    EXPECT_TRUE(maskOn == propertyOn);

    ASSERT_NE(maskPathRow->layout(), nullptr);
    ASSERT_NE(featherRow->layout(), nullptr);
    const QMargins maskMargins = maskPathRow->layout()->contentsMargins();
    const QMargins propertyMargins = featherRow->layout()->contentsMargins();
    EXPECT_EQ(maskMargins.left(), propertyMargins.left());
    EXPECT_EQ(maskMargins.top(), propertyMargins.top());
    EXPECT_EQ(maskMargins.right(), propertyMargins.right());
    EXPECT_EQ(maskMargins.bottom(), propertyMargins.bottom());
    EXPECT_EQ(maskPathRow->layout()->spacing(), featherRow->layout()->spacing());

    ASSERT_EQ(maskPathRow->parentWidget(), featherRow->parentWidget());
    QWidget* commonParent = maskPathRow->parentWidget();
    const int maskX = maskStopwatch->mapTo(commonParent, QPoint()).x();
    const int propertyX = propertyStopwatch->mapTo(commonParent, QPoint()).x();
    EXPECT_EQ(maskX, propertyX);
    EXPECT_EQ(maskX, 26);
}

TEST(EffectControlsPanel, MaskPathStopwatchAndDiamondCreateUndoableKeys)
{
    rt::VideoClip clip;
    clip.setDuration(48000);
    rt::OpacityMask mask;
    mask.name = "Mask 1";
    mask.shape = rt::MaskShape::FreeDrawBezier;
    mask.base.vertices = {
        {0.2f, 0.2f, 0.0f, 0.0f, 0.0f, 0.0f},
        {0.8f, 0.2f, 0.0f, 0.0f, 0.0f, 0.0f},
        {0.5f, 0.8f, 0.0f, 0.0f, 0.0f, 0.0f}
    };
    clip.addMask(mask);

    rt::CommandStack stack;
    rt::EffectControlsPanel panel;
    panel.setCommandStack(&stack);
    panel.setClip(&clip);
    panel.setPlayheadTick(12000);

    auto* stopwatch = panel.findChild<QToolButton*>(
        QStringLiteral("maskPathStopwatch"));
    auto* diamond = panel.findChild<QToolButton*>(
        QStringLiteral("maskPathKeyButton"));
    ASSERT_NE(stopwatch, nullptr);
    ASSERT_NE(diamond, nullptr);
    EXPECT_FALSE(stopwatch->isChecked());
    EXPECT_FALSE(diamond->isEnabled());

    stopwatch->click();
    ASSERT_TRUE(clip.masks()[0].pathAnimated);
    ASSERT_EQ(clip.masks()[0].pathKeys.size(), 1u);
    EXPECT_EQ(clip.masks()[0].pathKeys[0].time, 12000);
    EXPECT_TRUE(diamond->isEnabled());
    EXPECT_TRUE(diamond->isChecked());

    panel.setPlayheadTick(24000);
    EXPECT_FALSE(diamond->isChecked());
    diamond->click();
    ASSERT_EQ(clip.masks()[0].pathKeys.size(), 2u);
    EXPECT_EQ(clip.masks()[0].pathKeys[1].time, 24000);
    EXPECT_TRUE(diamond->isChecked());

    ASSERT_TRUE(stack.undo());
    EXPECT_EQ(clip.masks()[0].pathKeys.size(), 1u);
    ASSERT_TRUE(stack.redo());
    EXPECT_EQ(clip.masks()[0].pathKeys.size(), 2u);
}

TEST(EffectControlsPanel, MaskScalarStopwatchCreatesUndoableKeyframe)
{
    rt::VideoClip clip;
    clip.setDuration(48000);
    rt::OpacityMask mask;
    mask.name = "Mask 1";
    clip.addMask(mask);

    rt::CommandStack stack;
    rt::EffectControlsPanel panel;
    panel.setCommandStack(&stack);
    panel.setClip(&clip);
    panel.setPlayheadTick(18000);

    rt::PropertyRow* featherRow = nullptr;
    for (auto* row : panel.findChildren<rt::PropertyRow*>()) {
        if (row->propertyName() == QStringLiteral("Mask Feather")) {
            featherRow = row;
            break;
        }
    }
    ASSERT_NE(featherRow, nullptr);
    ASSERT_NE(featherRow->stopwatchButton(), nullptr);

    featherRow->stopwatchButton()->click();
    ASSERT_EQ(clip.masks()[0].feather.keyframeCount(), 1u);
    EXPECT_EQ(clip.masks()[0].feather.keyframe(0).time, 18000);

    ASSERT_TRUE(stack.undo());
    EXPECT_EQ(clip.masks()[0].feather.keyframeCount(), 0u);
    ASSERT_TRUE(stack.redo());
    EXPECT_EQ(clip.masks()[0].feather.keyframeCount(), 1u);
}

TEST(EffectControlsPanel, MaskScalarTimelineUndoSurvivesMaskVectorReallocation)
{
    rt::VideoClip clip;
    clip.setDuration(48000);
    rt::OpacityMask mask;
    mask.name = "Mask 1";
    mask.feather.addKeyframe(10000, 10.0f);
    mask.feather.addKeyframe(30000, 30.0f);
    clip.addMask(mask);
    const uint64_t stableId = clip.masks()[0].maskId;

    rt::PropertyRow row(QStringLiteral("Mask Feather"),
                        &clip.masks()[0].feather);
    row.move(0, 100);
    row.resize(200, 28);
    row.show();

    rt::CommandStack stack;
    rt::KeyframeTimeline timeline;
    timeline.setCommandStack(&stack);
    timeline.resize(481, 240);
    timeline.setClip(&clip);
    timeline.setPropertyRows({&row});
    timeline.show();
    QApplication::processEvents();

    QTest::mousePress(&timeline, Qt::LeftButton, Qt::NoModifier,
                      QPoint(100, 114));
    QTest::mouseMove(&timeline, QPoint(200, 114));
    QTest::mouseRelease(&timeline, Qt::LeftButton, Qt::NoModifier,
                        QPoint(200, 114));
    ASSERT_EQ(clip.masks()[0].feather.keyframe(0).time, 20000);

    // Force the OpacityMask vector to relocate after the command has captured
    // its target. Undo must resolve by maskId, never dereference the old row's
    // member pointer.
    for (int i = 0; i < 64; ++i) clip.addMask(rt::OpacityMask{});
    auto maskIt = std::find_if(clip.masks().begin(), clip.masks().end(),
        [stableId](const rt::OpacityMask& value) {
            return value.maskId == stableId;
        });
    ASSERT_NE(maskIt, clip.masks().end());
    ASSERT_TRUE(stack.undo());
    ASSERT_EQ(maskIt->feather.keyframeCount(), 2u);
    EXPECT_EQ(maskIt->feather.keyframe(0).time, 10000);
    ASSERT_TRUE(stack.redo());
    EXPECT_EQ(maskIt->feather.keyframe(0).time, 20000);
}

TEST(EffectControlsPanel, RemoveAllKeyframesIncludesMaskPathAndScalars)
{
    rt::VideoClip clip;
    clip.setDuration(48000);
    rt::OpacityMask mask;
    mask.name = "Mask 1";
    mask.pathAnimated = true;
    rt::MaskGeometry a;
    a.centerX = 0.2f;
    rt::MaskGeometry b;
    b.centerX = 0.8f;
    mask.addPathKey(0, a);
    mask.addPathKey(48000, b);
    mask.feather.addKeyframe(0, 4.0f);
    mask.feather.addKeyframe(48000, 20.0f);
    clip.addMask(mask);

    rt::CommandStack stack;
    rt::EffectControlsPanel panel;
    panel.setCommandStack(&stack);
    panel.setClip(&clip);
    panel.setPlayheadTick(24000);
    panel.removeAllKeyframes();

    ASSERT_EQ(clip.masks().size(), 1u);
    EXPECT_FALSE(clip.masks()[0].pathAnimated);
    EXPECT_TRUE(clip.masks()[0].pathKeys.empty());
    EXPECT_FLOAT_EQ(clip.masks()[0].base.centerX, 0.5f);
    EXPECT_EQ(clip.masks()[0].feather.keyframeCount(), 0u);
    EXPECT_FLOAT_EQ(clip.masks()[0].feather.defaultValue(), 12.0f);

    ASSERT_TRUE(stack.undo());
    EXPECT_TRUE(clip.masks()[0].pathAnimated);
    EXPECT_EQ(clip.masks()[0].pathKeys.size(), 2u);
    EXPECT_EQ(clip.masks()[0].feather.keyframeCount(), 2u);
    ASSERT_TRUE(stack.redo());
    EXPECT_TRUE(clip.masks()[0].pathKeys.empty());
}

TEST(EffectControlsPanel, MaskPathTimelineDrawsAndDragsGeometryKeyUndoably)
{
    rt::VideoClip clip;
    clip.setDuration(48000);
    rt::OpacityMask mask;
    mask.name = "Mask 1";
    mask.pathAnimated = true;
    rt::MaskGeometry first;
    first.centerX = 0.2f;
    rt::MaskGeometry second;
    second.centerX = 0.8f;
    mask.addPathKey(10000, first);
    mask.addPathKey(30000, second);
    clip.addMask(mask);

    QWidget row;
    row.move(0, 100);
    row.resize(200, 28);
    row.show();

    rt::CommandStack stack;
    rt::KeyframeTimeline timeline;
    timeline.setCommandStack(&stack);
    timeline.resize(481, 240); // 100 ticks per horizontal pixel
    timeline.setClip(&clip);
    timeline.setMaskPathLanes({{&row, 0, clip.masks()[0].maskId}});
    timeline.show();
    QApplication::processEvents();

    // The geometry lane paints a visible diamond at its first path key.
    const QImage painted = timeline.grab().toImage();
    EXPECT_NE(painted.pixelColor(100, 114), painted.pixelColor(115, 114));

    QTest::mousePress(&timeline, Qt::LeftButton, Qt::NoModifier,
                      QPoint(100, 114));
    QTest::mouseMove(&timeline, QPoint(200, 114));
    QTest::mouseRelease(&timeline, Qt::LeftButton, Qt::NoModifier,
                        QPoint(200, 114));

    ASSERT_EQ(clip.masks()[0].pathKeys.size(), 2u);
    EXPECT_EQ(clip.masks()[0].pathKeys[0].time, 20000);
    EXPECT_FLOAT_EQ(clip.masks()[0].pathKeys[0].geometry.centerX, 0.2f);
    ASSERT_EQ(stack.undoCount(), 1u);
    EXPECT_EQ(stack.undoDescription(), "Move Keyframes");

    ASSERT_TRUE(stack.undo());
    EXPECT_EQ(clip.masks()[0].pathKeys[0].time, 10000);
    ASSERT_TRUE(stack.redo());
    EXPECT_EQ(clip.masks()[0].pathKeys[0].time, 20000);
}

TEST(EffectControlsPanel, MaskPathTimelineClipboardDeleteAndUndoPreserveGeometry)
{
    rt::VideoClip clip;
    clip.setDuration(48000);
    rt::OpacityMask mask;
    mask.name = "Mask 1";
    mask.pathAnimated = true;
    rt::MaskGeometry geometry;
    geometry.centerX = 0.35f;
    geometry.centerY = 0.65f;
    geometry.vertices = {
        {0.1f, 0.2f, -0.05f, 0.0f, 0.05f, 0.0f},
        {0.8f, 0.2f,  0.0f, -0.1f, 0.0f, 0.1f},
        {0.5f, 0.9f,  0.1f, 0.0f, -0.1f, 0.0f}
    };
    mask.addPathKey(10000, geometry);
    clip.addMask(mask);

    QWidget row;
    row.move(0, 100);
    row.resize(200, 28);
    row.show();

    rt::CommandStack stack;
    rt::KeyframeTimeline timeline;
    timeline.setCommandStack(&stack);
    timeline.resize(481, 240);
    timeline.setClip(&clip);
    timeline.setMaskPathLanes({{&row, 0, clip.masks()[0].maskId}});
    timeline.show();
    QApplication::processEvents();

    QTest::mouseClick(&timeline, Qt::LeftButton, Qt::NoModifier,
                      QPoint(100, 114));
    ASSERT_TRUE(timeline.hasSelectedKeyframes());
    QTest::keyClick(&timeline, Qt::Key_C, Qt::ControlModifier);
    ASSERT_TRUE(timeline.hasKfClipboardData());

    timeline.setPlayheadTick(40000);
    QTest::keyClick(&timeline, Qt::Key_V, Qt::ControlModifier);
    ASSERT_EQ(clip.masks()[0].pathKeys.size(), 2u);
    ASSERT_EQ(clip.masks()[0].pathKeys[1].time, 40000);
    EXPECT_FLOAT_EQ(clip.masks()[0].pathKeys[1].geometry.centerX, 0.35f);
    ASSERT_EQ(clip.masks()[0].pathKeys[1].geometry.vertices.size(), 3u);
    EXPECT_FLOAT_EQ(clip.masks()[0].pathKeys[1].geometry.vertices[1].outTanY,
                    0.1f);

    QTest::keyClick(&timeline, Qt::Key_Delete);
    ASSERT_EQ(clip.masks().size(), 1u)
        << "Delete on a selected Mask Path diamond must keep its mask";
    ASSERT_EQ(clip.masks()[0].pathKeys.size(), 1u);
    EXPECT_EQ(stack.undoDescription(), "Delete Keyframes");
    ASSERT_TRUE(stack.undo());
    ASSERT_EQ(clip.masks()[0].pathKeys.size(), 2u);
    EXPECT_EQ(clip.masks()[0].pathKeys[1].time, 40000);

    ASSERT_TRUE(stack.undo()); // undo paste
    ASSERT_EQ(clip.masks()[0].pathKeys.size(), 1u);
    EXPECT_EQ(clip.masks()[0].pathKeys[0].time, 10000);
    ASSERT_TRUE(stack.redo());
    ASSERT_EQ(clip.masks()[0].pathKeys.size(), 2u);

    QTest::mouseClick(&timeline, Qt::LeftButton, Qt::NoModifier,
                      QPoint(100, 114));
    QTest::keyClick(&timeline, Qt::Key_X, Qt::ControlModifier);
    ASSERT_EQ(clip.masks()[0].pathKeys.size(), 1u);
    EXPECT_EQ(clip.masks()[0].pathKeys[0].time, 40000);
    EXPECT_EQ(stack.undoDescription(), "Cut Keyframes");
    ASSERT_TRUE(stack.undo());
    ASSERT_EQ(clip.masks()[0].pathKeys.size(), 2u);
    EXPECT_EQ(clip.masks()[0].pathKeys[0].time, 10000);
}

// ═══════════════════════════════════════════════════════════════════════════
//  PropertiesPanel — clear and switch clips
// ═══════════════════════════════════════════════════════════════════════════

TEST(PropertiesPanel, ClearClip)
{
    rt::PropertiesPanel panel;
    rt::SpineClip clip;

    panel.setClip(&clip);
    EXPECT_NE(panel.clip(), nullptr);

    panel.clearClip();
    EXPECT_EQ(panel.clip(), nullptr);
    EXPECT_EQ(panel.track(), nullptr);
}

TEST(PropertiesPanel, SwitchClipType)
{
    rt::PropertiesPanel panel;

    // First set a Spine clip
    rt::SpineClip spineClip;
    spineClip.setLabel("Spine");
    panel.setClip(&spineClip);
    EXPECT_EQ(panel.clip()->clipType(), rt::ClipType::Spine);
    EXPECT_EQ(panel.labelEdit()->text().toStdString(), "Spine");

    // Switch to a Video clip
    rt::VideoClip videoClip;
    videoClip.setLabel("Video");
    panel.setClip(&videoClip);
    EXPECT_EQ(panel.clip()->clipType(), rt::ClipType::Video);
    EXPECT_EQ(panel.labelEdit()->text().toStdString(), "Video");
}

TEST(PropertiesPanel, ClipChangedSignal)
{
    rt::PropertiesPanel panel;
    QSignalSpy spy(&panel, &rt::PropertiesPanel::clipChanged);

    rt::SpineClip clip;
    panel.setClip(&clip);
    EXPECT_EQ(spy.count(), 1);

    panel.clearClip();
    EXPECT_EQ(spy.count(), 2);
}

TEST(PropertiesPanel, RefreshReloadsValues)
{
    rt::PropertiesPanel panel;
    rt::VideoClip clip;
    clip.setLabel("Before");
    clip.setSpeed(1.0);

    panel.setClip(&clip);
    EXPECT_EQ(panel.labelEdit()->text().toStdString(), "Before");

    // Externally change the clip
    clip.setLabel("After");
    clip.setSpeed(3.0);

    // Refresh should reload
    panel.refresh();
    EXPECT_EQ(panel.labelEdit()->text().toStdString(), "After");
    EXPECT_DOUBLE_EQ(panel.speedSpin()->value(), 300.0);
}

TEST(PropertiesPanel, NoSpuriousSignalOnLoad)
{
    rt::PropertiesPanel panel;
    rt::SpineClip clip;
    clip.setLabel("Test");
    clip.setLooping(true);
    clip.setTalking(false);

    QSignalSpy spy(&panel, &rt::PropertiesPanel::propertyChanged);

    panel.setClip(&clip);

    // Setting clip should NOT trigger propertyChanged
    // (m_updating flag should prevent it)
    EXPECT_EQ(spy.count(), 0);
}

// ═══════════════════════════════════════════════════════════════════════════
//  PropertiesPanel — widget existence per type
// ═══════════════════════════════════════════════════════════════════════════

TEST(PropertiesPanel, SpineWidgetsExist)
{
    rt::PropertiesPanel panel;
    EXPECT_NE(panel.characterCombo(), nullptr);
    EXPECT_NE(panel.outfitCombo(), nullptr);
    EXPECT_NE(panel.stanceCombo(), nullptr);
    EXPECT_NE(panel.animationCombo(), nullptr);
    EXPECT_NE(panel.loopingCheck(), nullptr);
    EXPECT_NE(panel.talkingCheck(), nullptr);
    EXPECT_NE(panel.animSpeedSpin(), nullptr);
}

TEST(PropertiesPanel, VideoWidgetsExist)
{
    rt::PropertiesPanel panel;
    EXPECT_NE(panel.mediaPathLabel(), nullptr);
    EXPECT_NE(panel.volumeSpin(), nullptr);
}

TEST(PropertiesPanel, AudioWidgetsExist)
{
    rt::PropertiesPanel panel;
    EXPECT_NE(panel.audioVolumeSpin(), nullptr);
    EXPECT_NE(panel.panSpin(), nullptr);
    EXPECT_NE(panel.fadeInSpin(), nullptr);
    EXPECT_NE(panel.fadeOutSpin(), nullptr);
}

TEST(PropertiesPanel, TitleWidgetsExist)
{
    rt::PropertiesPanel panel;
    EXPECT_NE(panel.textEdit(), nullptr);
    EXPECT_NE(panel.fontFamilyEdit(), nullptr);
    EXPECT_NE(panel.fontSizeSpin(), nullptr);
    EXPECT_NE(panel.boldCheck(), nullptr);
    EXPECT_NE(panel.italicCheck(), nullptr);
    EXPECT_NE(panel.alignCombo(), nullptr);
}
