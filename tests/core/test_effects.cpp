/*
 * test_effects.cpp — Tests for Step 22: Effects System
 *
 * Tests Effect base class, EffectStack, all built-in effects,
 * EffectCommands (undo/redo), factory, evaluation, and cloning.
 */

#include <gtest/gtest.h>

#include "effects/Effect.h"
#include "effects/EffectStack.h"
#include "effects/ColorCorrect.h"
#include "effects/Blur.h"
#include "effects/Sharpen.h"
#include "effects/Glow.h"
#include "effects/ChromaKey.h"
#include "effects/Transform2D.h"
#include "effects/Tint.h"
#include "effects/ColorGrading.h"
#include "command/CommandStack.h"
#include "command/commands/EffectCommands.h"

#include <cmath>
#include <memory>

using namespace rt;

// ═══════════════════════════════════════════════════════════════════════════
//  Effect base class tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(EffectTest, ColorCorrectConstruction)
{
    ColorCorrect fx;
    EXPECT_EQ(fx.effectType(), EffectType::ColorCorrect);
    EXPECT_STREQ(fx.name(), "Color Correct");
    EXPECT_TRUE(fx.isEnabled());
    EXPECT_EQ(fx.paramCount(), 9u); // brightness, contrast, saturation, hue, temp, tint, gamma, gain, lift
}

TEST(EffectTest, BlurConstruction)
{
    Blur fx;
    EXPECT_EQ(fx.effectType(), EffectType::Blur);
    EXPECT_STREQ(fx.name(), "Gaussian Blur");
    EXPECT_EQ(fx.paramCount(), 1u); // radius only (sigma derived in shader as radius/3)
}

TEST(EffectTest, SharpenConstruction)
{
    Sharpen fx;
    EXPECT_EQ(fx.effectType(), EffectType::Sharpen);
    EXPECT_STREQ(fx.name(), "Sharpen");
    EXPECT_EQ(fx.paramCount(), 3u); // amount, radius, threshold
}

TEST(EffectTest, GlowConstruction)
{
    Glow fx;
    EXPECT_EQ(fx.effectType(), EffectType::Glow);
    EXPECT_STREQ(fx.name(), "Glow");
    EXPECT_EQ(fx.paramCount(), 3u); // threshold, intensity, radius
}

TEST(EffectTest, ChromaKeyConstruction)
{
    ChromaKey fx;
    EXPECT_EQ(fx.effectType(), EffectType::ChromaKey);
    EXPECT_STREQ(fx.name(), "Ultra Key");
    EXPECT_EQ(fx.paramCount(), 21u);
}

TEST(EffectTest, Transform2DConstruction)
{
    Transform2D fx;
    EXPECT_EQ(fx.effectType(), EffectType::Transform2D);
    EXPECT_STREQ(fx.name(), "Transform 2D");
    EXPECT_EQ(fx.paramCount(), 6u); // offsetX, offsetY, scale, rotation, anchorX, anchorY
}

TEST(EffectTest, TintConstructionAndPremiereDefaults)
{
    Tint fx;
    EXPECT_EQ(fx.effectType(), EffectType::Tint);
    EXPECT_STREQ(fx.name(), "Tint");
    ASSERT_EQ(fx.paramCount(), Tint::ParamCount);
    EXPECT_FLOAT_EQ(fx.evalParam(Tint::MapBlackR, 0), 0.0f);
    EXPECT_FLOAT_EQ(fx.evalParam(Tint::MapBlackG, 0), 0.0f);
    EXPECT_FLOAT_EQ(fx.evalParam(Tint::MapBlackB, 0), 0.0f);
    EXPECT_FLOAT_EQ(fx.evalParam(Tint::MapWhiteR, 0), 1.0f);
    EXPECT_FLOAT_EQ(fx.evalParam(Tint::MapWhiteG, 0), 1.0f);
    EXPECT_FLOAT_EQ(fx.evalParam(Tint::MapWhiteB, 0), 1.0f);
    EXPECT_FLOAT_EQ(fx.evalParam(Tint::AmountToTint, 0), 100.0f);
}

TEST(EffectTest, EnableDisable)
{
    ColorCorrect fx;
    EXPECT_TRUE(fx.isEnabled());
    fx.setEnabled(false);
    EXPECT_FALSE(fx.isEnabled());
    fx.setEnabled(true);
    EXPECT_TRUE(fx.isEnabled());
}

TEST(EffectTest, UniqueId)
{
    ColorCorrect a;
    Blur b;
    Sharpen c;
    EXPECT_NE(a.id(), b.id());
    EXPECT_NE(b.id(), c.id());
    EXPECT_NE(a.id(), c.id());
}

TEST(EffectTest, ParamDefaults)
{
    ColorCorrect fx;
    // brightness default = 0
    EXPECT_FLOAT_EQ(fx.evalParam(ColorCorrect::Brightness, 0), 0.0f);
    // contrast default = 1
    EXPECT_FLOAT_EQ(fx.evalParam(ColorCorrect::Contrast, 0), 1.0f);
    // saturation default = 1
    EXPECT_FLOAT_EQ(fx.evalParam(ColorCorrect::Saturation, 0), 1.0f);
}

TEST(EffectTest, ParamMinMax)
{
    ColorCorrect fx;
    EXPECT_FLOAT_EQ(fx.param(ColorCorrect::Brightness).minVal, -1.0f);
    EXPECT_FLOAT_EQ(fx.param(ColorCorrect::Brightness).maxVal, 1.0f);
    EXPECT_FLOAT_EQ(fx.param(ColorCorrect::Contrast).minVal, 0.0f);
    EXPECT_FLOAT_EQ(fx.param(ColorCorrect::Contrast).maxVal, 3.0f);
}

TEST(EffectTest, ParamName)
{
    Blur fx;
    EXPECT_EQ(fx.param(Blur::Radius).name, "Radius");
    // Sigma is derived inside the shader as radius / 3.0 — no longer a param.
}

TEST(EffectTest, FindParamByName)
{
    ColorCorrect fx;
    auto* p = fx.findParam("Brightness");
    ASSERT_NE(p, nullptr);
    EXPECT_FLOAT_EQ(p->track.evaluate(0), 0.0f);

    auto* missing = fx.findParam("NonExistent");
    EXPECT_EQ(missing, nullptr);
}

TEST(EffectTest, FindParamConst)
{
    const ColorCorrect fx;
    auto* p = fx.findParam("Contrast");
    ASSERT_NE(p, nullptr);
    EXPECT_FLOAT_EQ(p->track.evaluate(0), 1.0f);
}

TEST(EffectTest, EvalAllParams)
{
    Blur fx;
    auto vals = fx.evalAllParams(0);
    ASSERT_EQ(vals.size(), 1u);
    EXPECT_FLOAT_EQ(vals[0], 15.0f);  // radius default
}

TEST(EffectTest, KeyframeableParam)
{
    ColorCorrect fx;
    fx.param(ColorCorrect::Brightness).track.addKeyframe(0, 0.0f);
    fx.param(ColorCorrect::Brightness).track.addKeyframe(48000, 0.5f);

    EXPECT_FLOAT_EQ(fx.evalParam(ColorCorrect::Brightness, 0), 0.0f);
    EXPECT_FLOAT_EQ(fx.evalParam(ColorCorrect::Brightness, 48000), 0.5f);
    // Midpoint should interpolate
    float mid = fx.evalParam(ColorCorrect::Brightness, 24000);
    EXPECT_GT(mid, 0.0f);
    EXPECT_LT(mid, 0.5f);
}

TEST(EffectTest, CloneColorCorrect)
{
    ColorCorrect fx;
    fx.setEnabled(false);
    fx.param(ColorCorrect::Brightness).track.addKeyframe(0, 0.3f);

    auto cloned = fx.clone();
    ASSERT_NE(cloned, nullptr);
    EXPECT_EQ(cloned->effectType(), EffectType::ColorCorrect);
    EXPECT_FALSE(cloned->isEnabled());
    EXPECT_FLOAT_EQ(cloned->evalParam(ColorCorrect::Brightness, 0), 0.3f);
    // Clone has different ID
    EXPECT_NE(cloned->id(), fx.id());
}

TEST(EffectTest, CloneBlur)
{
    Blur fx;
    fx.param(Blur::Radius).track.addKeyframe(0, 10.0f);
    auto cloned = fx.clone();
    EXPECT_EQ(cloned->effectType(), EffectType::Blur);
    EXPECT_FLOAT_EQ(cloned->evalParam(Blur::Radius, 0), 10.0f);
}

TEST(EffectTest, CloneSharpen)
{
    Sharpen fx;
    auto cloned = fx.clone();
    EXPECT_EQ(cloned->effectType(), EffectType::Sharpen);
    EXPECT_EQ(cloned->paramCount(), 3u);
}

TEST(EffectTest, CloneGlow)
{
    Glow fx;
    auto cloned = fx.clone();
    EXPECT_EQ(cloned->effectType(), EffectType::Glow);
}

TEST(EffectTest, CloneChromaKey)
{
    ChromaKey fx;
    auto cloned = fx.clone();
    EXPECT_EQ(cloned->effectType(), EffectType::ChromaKey);
    EXPECT_EQ(cloned->paramCount(), 21u);
}

TEST(EffectTest, CloneTransform2D)
{
    Transform2D fx;
    auto cloned = fx.clone();
    EXPECT_EQ(cloned->effectType(), EffectType::Transform2D);
}

TEST(EffectTest, CloneTintPreservesMappedColorsAndAmount)
{
    Tint fx;
    fx.param(Tint::MapBlackR).track.setDefaultValue(0.25f);
    fx.param(Tint::MapWhiteB).track.setDefaultValue(0.75f);
    fx.param(Tint::AmountToTint).track.setDefaultValue(40.0f);

    auto cloned = fx.clone();
    ASSERT_NE(cloned, nullptr);
    EXPECT_EQ(cloned->effectType(), EffectType::Tint);
    EXPECT_FLOAT_EQ(cloned->evalParam(Tint::MapBlackR, 0), 0.25f);
    EXPECT_FLOAT_EQ(cloned->evalParam(Tint::MapWhiteB, 0), 0.75f);
    EXPECT_FLOAT_EQ(cloned->evalParam(Tint::AmountToTint, 0), 40.0f);
}

TEST(EffectTest, ColorGradingStateAndGpuPassesCoverAllSections)
{
    auto grading = std::make_unique<ColorGrading>();
    grading->setBasicEnabled(false);
    grading->setWheelsEnabled(false);
    grading->param(ColorGrading::Temperature).track.setDefaultValue(75.0f);
    grading->param(ColorGrading::Saturation).track.setDefaultValue(25.0f);
    grading->param(ColorGrading::Sharpen).track.setDefaultValue(40.0f);

    ColorGrading::CurveLUT red = grading->curveLUT(ColorGrading::CurveRed);
    red[51] = 0.35f;
    grading->setCurveLUT(ColorGrading::CurveRed, red);

    ColorGrading::HslParams hsl;
    hsl.hueCenter = 120.0f;
    hsl.hueShift = 45.0f;
    grading->setHslParams(hsl);

    auto saved = grading->captureState();
    grading->setBasicEnabled(true);
    grading->setHslParams({});
    grading->restoreState(saved);
    EXPECT_FALSE(grading->basicEnabled());
    EXPECT_FLOAT_EQ(grading->hslParams().hueShift, 45.0f);
    EXPECT_FLOAT_EQ(grading->curveLUT(ColorGrading::CurveRed)[51], 0.35f);

    EffectStack stack;
    stack.addEffect(std::move(grading));
    const auto passes = stack.evaluate(0);
    ASSERT_EQ(passes.size(), 4u); // base, curves, HSL, sharpen
    EXPECT_EQ(passes[0].type, EffectType::ColorGrading);
    ASSERT_EQ(passes[0].params.size(), 28u);
    EXPECT_FLOAT_EQ(passes[0].params[0], 0.0f);   // Basic bypassed
    EXPECT_FLOAT_EQ(passes[0].params[8], 100.0f); // neutral saturation
    EXPECT_EQ(passes[1].params.size(), 24u);
    EXPECT_EQ(passes[2].params.size(), 9u);
    EXPECT_EQ(passes[3].type, EffectType::Sharpen);
    ASSERT_EQ(passes[3].params.size(), 3u);
    EXPECT_FLOAT_EQ(passes[3].params[0], 2.0f);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Factory tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(EffectFactoryTest, CreateAllTypes)
{
    for (int i = 0; i < static_cast<int>(EffectType::Count); ++i) {
        const auto type = static_cast<EffectType>(i);
        auto fx = createEffect(type);
        ASSERT_NE(fx, nullptr) << "Failed for type " << i;
        if (type == EffectType::LumetriColor) {
            // Legacy alias kept only for serialized-project numbering: it is
            // intentionally constructed as ColorGrading (see createEffect).
            EXPECT_EQ(fx->effectType(), EffectType::ColorGrading);
        } else {
            EXPECT_EQ(fx->effectType(), type);
        }
    }
}

TEST(EffectFactoryTest, InvalidType)
{
    auto fx = createEffect(EffectType::Count);
    EXPECT_EQ(fx, nullptr);
}

TEST(EffectFactoryTest, EffectTypeNames)
{
    EXPECT_STREQ(effectTypeName(EffectType::ColorCorrect), "Color Correct");
    EXPECT_STREQ(effectTypeName(EffectType::Blur), "Gaussian Blur");
    EXPECT_STREQ(effectTypeName(EffectType::Sharpen), "Sharpen");
    EXPECT_STREQ(effectTypeName(EffectType::Glow), "Glow");
    EXPECT_STREQ(effectTypeName(EffectType::ChromaKey), "Ultra Key");
    EXPECT_STREQ(effectTypeName(EffectType::Transform2D), "Transform 2D");
    EXPECT_STREQ(effectTypeName(EffectType::Tint), "Tint");
}

// ═══════════════════════════════════════════════════════════════════════════
//  EffectStack tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(EffectStackTest, DefaultEmpty)
{
    EffectStack stack;
    EXPECT_EQ(stack.effectCount(), 0u);
    EXPECT_TRUE(stack.isEmpty());
    EXPECT_FALSE(stack.hasActiveEffects());
}

TEST(EffectStackTest, AddEffect)
{
    EffectStack stack;
    stack.addEffect(std::make_unique<ColorCorrect>());
    EXPECT_EQ(stack.effectCount(), 1u);
    EXPECT_FALSE(stack.isEmpty());
    EXPECT_EQ(stack.effect(0).effectType(), EffectType::ColorCorrect);
}

TEST(EffectStackTest, AddMultiple)
{
    EffectStack stack;
    stack.addEffect(std::make_unique<ColorCorrect>());
    stack.addEffect(std::make_unique<Blur>());
    stack.addEffect(std::make_unique<Sharpen>());
    EXPECT_EQ(stack.effectCount(), 3u);
    EXPECT_EQ(stack.effect(0).effectType(), EffectType::ColorCorrect);
    EXPECT_EQ(stack.effect(1).effectType(), EffectType::Blur);
    EXPECT_EQ(stack.effect(2).effectType(), EffectType::Sharpen);
}

TEST(EffectStackTest, InsertEffect)
{
    EffectStack stack;
    stack.addEffect(std::make_unique<ColorCorrect>());
    stack.addEffect(std::make_unique<Sharpen>());
    stack.insertEffect(1, std::make_unique<Blur>());
    EXPECT_EQ(stack.effectCount(), 3u);
    EXPECT_EQ(stack.effect(0).effectType(), EffectType::ColorCorrect);
    EXPECT_EQ(stack.effect(1).effectType(), EffectType::Blur);
    EXPECT_EQ(stack.effect(2).effectType(), EffectType::Sharpen);
}

TEST(EffectStackTest, InsertBeyondEnd)
{
    EffectStack stack;
    stack.insertEffect(99, std::make_unique<Blur>());
    EXPECT_EQ(stack.effectCount(), 1u);
    EXPECT_EQ(stack.effect(0).effectType(), EffectType::Blur);
}

TEST(EffectStackTest, RemoveEffect)
{
    EffectStack stack;
    stack.addEffect(std::make_unique<ColorCorrect>());
    stack.addEffect(std::make_unique<Blur>());
    auto removed = stack.removeEffect(0);
    ASSERT_NE(removed, nullptr);
    EXPECT_EQ(removed->effectType(), EffectType::ColorCorrect);
    EXPECT_EQ(stack.effectCount(), 1u);
    EXPECT_EQ(stack.effect(0).effectType(), EffectType::Blur);
}

TEST(EffectStackTest, RemoveInvalid)
{
    EffectStack stack;
    auto removed = stack.removeEffect(0);
    EXPECT_EQ(removed, nullptr);
}

TEST(EffectStackTest, MoveEffect)
{
    EffectStack stack;
    stack.addEffect(std::make_unique<ColorCorrect>());
    stack.addEffect(std::make_unique<Blur>());
    stack.addEffect(std::make_unique<Sharpen>());

    // Move Sharpen (index 2) to index 0
    stack.moveEffect(2, 0);
    EXPECT_EQ(stack.effect(0).effectType(), EffectType::Sharpen);
    EXPECT_EQ(stack.effect(1).effectType(), EffectType::ColorCorrect);
    EXPECT_EQ(stack.effect(2).effectType(), EffectType::Blur);
}

TEST(EffectStackTest, MoveEffectSameIndex)
{
    EffectStack stack;
    stack.addEffect(std::make_unique<ColorCorrect>());
    stack.addEffect(std::make_unique<Blur>());
    stack.moveEffect(0, 0); // no-op
    EXPECT_EQ(stack.effect(0).effectType(), EffectType::ColorCorrect);
}

TEST(EffectStackTest, FindById)
{
    EffectStack stack;
    stack.addEffect(std::make_unique<ColorCorrect>());
    stack.addEffect(std::make_unique<Blur>());

    auto targetId = stack.effect(1).id();
    auto* found = stack.effectById(targetId);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->effectType(), EffectType::Blur);
}

TEST(EffectStackTest, FindByIdNotFound)
{
    EffectStack stack;
    EXPECT_EQ(stack.effectById(999), nullptr);
}

TEST(EffectStackTest, IndexOf)
{
    EffectStack stack;
    stack.addEffect(std::make_unique<ColorCorrect>());
    stack.addEffect(std::make_unique<Blur>());

    auto id0 = stack.effect(0).id();
    auto id1 = stack.effect(1).id();

    EXPECT_EQ(stack.indexOf(id0), 0u);
    EXPECT_EQ(stack.indexOf(id1), 1u);
    EXPECT_EQ(stack.indexOf(999), 2u); // not found = effectCount
}

TEST(EffectStackTest, HasActiveEffects)
{
    EffectStack stack;
    EXPECT_FALSE(stack.hasActiveEffects());

    stack.addEffect(std::make_unique<ColorCorrect>());
    EXPECT_TRUE(stack.hasActiveEffects());

    stack.effect(0).setEnabled(false);
    EXPECT_FALSE(stack.hasActiveEffects());
}

TEST(EffectStackTest, Evaluate)
{
    EffectStack stack;
    stack.addEffect(std::make_unique<Blur>());
    stack.addEffect(std::make_unique<Glow>());

    auto snapshots = stack.evaluate(0);
    ASSERT_EQ(snapshots.size(), 2u);
    EXPECT_EQ(snapshots[0].type, EffectType::Blur);
    EXPECT_EQ(snapshots[0].params.size(), 1u);
    EXPECT_EQ(snapshots[1].type, EffectType::Glow);
    EXPECT_EQ(snapshots[1].params.size(), 3u);
}

TEST(EffectStackTest, EvaluateSkipsDisabled)
{
    EffectStack stack;
    stack.addEffect(std::make_unique<Blur>());
    stack.addEffect(std::make_unique<Glow>());
    stack.effect(0).setEnabled(false);

    auto snapshots = stack.evaluate(0);
    ASSERT_EQ(snapshots.size(), 1u);
    EXPECT_EQ(snapshots[0].type, EffectType::Glow);
}

TEST(EffectStackTest, CloneStack)
{
    EffectStack stack;
    stack.addEffect(std::make_unique<ColorCorrect>());
    stack.addEffect(std::make_unique<Blur>());
    stack.effect(1).setEnabled(false);

    auto cloned = stack.clone();
    ASSERT_NE(cloned, nullptr);
    EXPECT_EQ(cloned->effectCount(), 2u);
    EXPECT_EQ(cloned->effect(0).effectType(), EffectType::ColorCorrect);
    EXPECT_EQ(cloned->effect(1).effectType(), EffectType::Blur);
    EXPECT_FALSE(cloned->effect(1).isEnabled());
}

// ═══════════════════════════════════════════════════════════════════════════
//  EffectCommands tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(EffectCommandTest, AddEffectCommand)
{
    EffectStack stack;
    CommandStack cmds;

    cmds.execute(std::make_unique<AddEffectCommand>(&stack, EffectType::Blur));
    EXPECT_EQ(stack.effectCount(), 1u);
    EXPECT_EQ(stack.effect(0).effectType(), EffectType::Blur);

    cmds.undo();
    EXPECT_EQ(stack.effectCount(), 0u);

    cmds.redo();
    EXPECT_EQ(stack.effectCount(), 1u);
    EXPECT_EQ(stack.effect(0).effectType(), EffectType::Blur);
}

TEST(EffectCommandTest, AddEffectDescription)
{
    EffectStack stack;
    auto cmd = std::make_unique<AddEffectCommand>(&stack, EffectType::ColorCorrect);
    EXPECT_EQ(cmd->description(), "Add Color Correct");
}

TEST(EffectCommandTest, RemoveEffectCommand)
{
    EffectStack stack;
    stack.addEffect(std::make_unique<ColorCorrect>());
    stack.addEffect(std::make_unique<Blur>());

    CommandStack cmds;
    cmds.execute(std::make_unique<RemoveEffectCommand>(&stack, 0));
    EXPECT_EQ(stack.effectCount(), 1u);
    EXPECT_EQ(stack.effect(0).effectType(), EffectType::Blur);

    cmds.undo();
    EXPECT_EQ(stack.effectCount(), 2u);
    EXPECT_EQ(stack.effect(0).effectType(), EffectType::ColorCorrect);
    EXPECT_EQ(stack.effect(1).effectType(), EffectType::Blur);
}

TEST(EffectCommandTest, MoveEffectCommand)
{
    EffectStack stack;
    stack.addEffect(std::make_unique<ColorCorrect>());
    stack.addEffect(std::make_unique<Blur>());
    stack.addEffect(std::make_unique<Sharpen>());

    CommandStack cmds;
    cmds.execute(std::make_unique<MoveEffectCommand>(&stack, 2, 0));
    EXPECT_EQ(stack.effect(0).effectType(), EffectType::Sharpen);
    EXPECT_EQ(stack.effect(1).effectType(), EffectType::ColorCorrect);
    EXPECT_EQ(stack.effect(2).effectType(), EffectType::Blur);

    cmds.undo();
    EXPECT_EQ(stack.effect(0).effectType(), EffectType::ColorCorrect);
    EXPECT_EQ(stack.effect(1).effectType(), EffectType::Blur);
    EXPECT_EQ(stack.effect(2).effectType(), EffectType::Sharpen);
}

TEST(EffectCommandTest, SetParamCommand)
{
    EffectStack stack;
    stack.addEffect(std::make_unique<Blur>());
    auto id = stack.effect(0).id();

    CommandStack cmds;
    cmds.execute(std::make_unique<SetEffectParamCommand>(&stack, id, Blur::Radius, 20.0f));
    EXPECT_FLOAT_EQ(stack.effect(0).evalParam(Blur::Radius, 0), 20.0f);

    cmds.undo();
    EXPECT_FLOAT_EQ(stack.effect(0).evalParam(Blur::Radius, 0), 15.0f); // default
}

TEST(EffectCommandTest, SetParamMerge)
{
    EffectStack stack;
    stack.addEffect(std::make_unique<Blur>());
    auto id = stack.effect(0).id();

    CommandStack cmds;
    cmds.execute(std::make_unique<SetEffectParamCommand>(&stack, id, Blur::Radius, 10.0f));
    cmds.execute(std::make_unique<SetEffectParamCommand>(&stack, id, Blur::Radius, 15.0f));
    cmds.execute(std::make_unique<SetEffectParamCommand>(&stack, id, Blur::Radius, 20.0f));

    EXPECT_FLOAT_EQ(stack.effect(0).evalParam(Blur::Radius, 0), 20.0f);

    // All merged into one undo step
    cmds.undo();
    EXPECT_FLOAT_EQ(stack.effect(0).evalParam(Blur::Radius, 0), 15.0f);
}

TEST(EffectCommandTest, SetParamNoMergeDifferentParam)
{
    EffectStack stack;
    // Blur is single-param now, so exercise no-merge with a multi-param effect:
    // setting two DIFFERENT params must produce two separate undo steps.
    stack.addEffect(std::make_unique<Sharpen>());
    auto id = stack.effect(0).id();

    CommandStack cmds;
    cmds.execute(std::make_unique<SetEffectParamCommand>(&stack, id, Sharpen::Amount, 2.0f));
    cmds.execute(std::make_unique<SetEffectParamCommand>(&stack, id, Sharpen::Radius, 3.0f));

    // Different params don't merge — one undo reverts only the most recent (Radius).
    cmds.undo();
    EXPECT_FLOAT_EQ(stack.effect(0).evalParam(Sharpen::Radius, 0), 1.0f); // back to default
    EXPECT_FLOAT_EQ(stack.effect(0).evalParam(Sharpen::Amount, 0), 2.0f); // still changed
}

TEST(EffectCommandTest, SetEffectEnabledCommand)
{
    EffectStack stack;
    stack.addEffect(std::make_unique<ColorCorrect>());
    auto id = stack.effect(0).id();

    CommandStack cmds;
    cmds.execute(std::make_unique<SetEffectEnabledCommand>(&stack, id, false));
    EXPECT_FALSE(stack.effect(0).isEnabled());

    cmds.undo();
    EXPECT_TRUE(stack.effect(0).isEnabled());

    cmds.redo();
    EXPECT_FALSE(stack.effect(0).isEnabled());
}

TEST(EffectCommandTest, AddRemoveRoundTrip)
{
    EffectStack stack;
    CommandStack cmds;

    cmds.execute(std::make_unique<AddEffectCommand>(&stack, EffectType::Glow));
    cmds.execute(std::make_unique<AddEffectCommand>(&stack, EffectType::ChromaKey));
    EXPECT_EQ(stack.effectCount(), 2u);

    cmds.execute(std::make_unique<RemoveEffectCommand>(&stack, 0));
    EXPECT_EQ(stack.effectCount(), 1u);
    EXPECT_EQ(stack.effect(0).effectType(), EffectType::ChromaKey);

    cmds.undo(); // undo remove
    EXPECT_EQ(stack.effectCount(), 2u);

    cmds.undo(); // undo add ChromaKey
    EXPECT_EQ(stack.effectCount(), 1u);

    cmds.undo(); // undo add Glow
    EXPECT_EQ(stack.effectCount(), 0u);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Built-in effect parameter checks
// ═══════════════════════════════════════════════════════════════════════════

TEST(EffectParamsTest, BlurDefaults)
{
    Blur fx;
    EXPECT_FLOAT_EQ(fx.evalParam(Blur::Radius, 0), 15.0f);
    // Sigma is derived as radius/3.0 — not a user-facing param.
    EXPECT_FLOAT_EQ(fx.param(Blur::Radius).minVal, 0.0f);
    EXPECT_FLOAT_EQ(fx.param(Blur::Radius).maxVal, 100.0f);
}

TEST(EffectParamsTest, SharpenDefaults)
{
    Sharpen fx;
    EXPECT_FLOAT_EQ(fx.evalParam(Sharpen::Amount, 0), 1.0f);
    EXPECT_FLOAT_EQ(fx.evalParam(Sharpen::Radius, 0), 1.0f);
    EXPECT_FLOAT_EQ(fx.evalParam(Sharpen::Threshold, 0), 0.0f);
}

TEST(EffectParamsTest, GlowDefaults)
{
    Glow fx;
    EXPECT_FLOAT_EQ(fx.evalParam(Glow::GlowThreshold, 0), 0.8f);
    EXPECT_FLOAT_EQ(fx.evalParam(Glow::Intensity, 0), 1.0f);
    EXPECT_FLOAT_EQ(fx.evalParam(Glow::GlowRadius, 0), 10.0f);
}

TEST(EffectParamsTest, ChromaKeyDefaults)
{
    ChromaKey fx;
    EXPECT_FLOAT_EQ(fx.evalParam(ChromaKey::KeyColorR, 0), 0.0f);
    EXPECT_FLOAT_EQ(fx.evalParam(ChromaKey::KeyColorG, 0), 1.0f);
    EXPECT_FLOAT_EQ(fx.evalParam(ChromaKey::KeyColorB, 0), 0.0f);
    EXPECT_FLOAT_EQ(fx.evalParam(ChromaKey::Transparency, 0), 45.0f);
    EXPECT_FLOAT_EQ(fx.evalParam(ChromaKey::Tolerance, 0), 50.0f);
    EXPECT_FLOAT_EQ(fx.evalParam(ChromaKey::Pedestal, 0), 10.0f);
    EXPECT_FLOAT_EQ(fx.evalParam(ChromaKey::Desaturate, 0), 25.0f);
    EXPECT_FLOAT_EQ(fx.evalParam(ChromaKey::Saturation, 0), 100.0f);
    EXPECT_FLOAT_EQ(fx.evalParam(ChromaKey::Hue, 0), 0.0f);
    EXPECT_FLOAT_EQ(fx.evalParam(ChromaKey::Luminance, 0), 100.0f);
}

TEST(EffectParamsTest, Transform2DDefaults)
{
    Transform2D fx;
    EXPECT_FLOAT_EQ(fx.evalParam(Transform2D::OffsetX, 0), 0.0f);
    EXPECT_FLOAT_EQ(fx.evalParam(Transform2D::Scale, 0), 1.0f);
    EXPECT_FLOAT_EQ(fx.evalParam(Transform2D::AnchorX, 0), 0.5f);
    EXPECT_FLOAT_EQ(fx.evalParam(Transform2D::AnchorY, 0), 0.5f);
}

TEST(EffectParamsTest, ColorCorrectGammaDefault)
{
    ColorCorrect fx;
    EXPECT_FLOAT_EQ(fx.evalParam(ColorCorrect::Gamma, 0), 1.0f);
    EXPECT_FLOAT_EQ(fx.evalParam(ColorCorrect::Gain, 0), 1.0f);
    EXPECT_FLOAT_EQ(fx.evalParam(ColorCorrect::Lift, 0), 0.0f);
}
