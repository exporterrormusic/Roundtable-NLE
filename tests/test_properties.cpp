/*
 * test_properties.cpp — Tests for Step 17: Properties Panel
 *
 * Tests ScrubbySpinBox and PropertiesPanel with all clip types.
 */

#include <gtest/gtest.h>

#include "widgets/ScrubbySpinBox.h"
#include "panels/properties/PropertiesPanel.h"

#include "timeline/Clip.h"
#include "timeline/SpineClip.h"
#include "timeline/VideoClip.h"
#include "timeline/AudioClip.h"
#include "timeline/TitleClip.h"
#include "timeline/Track.h"
#include "command/CommandStack.h"

#include <QApplication>
#include <QSignalSpy>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QTest>

#include <memory>

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
    EXPECT_DOUBLE_EQ(panel.speedSpin()->value(), 2.0);

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
    panel.characterCombo()->setCurrentText("Dorothy");
    EXPECT_EQ(clip.characterName(), "Dorothy");
    EXPECT_GE(spy.count(), 1);

    // Change animation
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
    EXPECT_DOUBLE_EQ(panel.speedSpin()->value(), 1.5);
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

    panel.speedSpin()->setValue(2.5);
    panel.speedSpin()->editingFinished();

    EXPECT_DOUBLE_EQ(clip.speed(), 2.5);
}

TEST(PropertiesPanel, TransformChange)
{
    rt::PropertiesPanel panel;
    rt::VideoClip clip;

    panel.setClip(&clip);

    panel.posXSpin()->setValue(100.0);
    panel.posYSpin()->setValue(200.0);
    panel.scaleXSpin()->setValue(150.0);   // 150% → internal 1.5
    panel.scaleYSpin()->setValue(80.0);    //  80% → internal 0.8
    panel.rotationSpin()->setValue(45.0);
    panel.opacitySpin()->setValue(70.0);   //  70% → internal 0.7

    // Trigger applyTransform
    panel.posXSpin()->editingFinished();

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
    EXPECT_DOUBLE_EQ(panel.speedSpin()->value(), 3.0);
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
