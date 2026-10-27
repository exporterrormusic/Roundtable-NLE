/*
 * SpineEngine.cpp — Skeleton loading, animation, mesh extraction.
 */

#ifdef ROUNDTABLE_HAS_SPINE

#include "spine/SpineEngine.h"

#include <spine/spine.h>
#include <spine/Atlas.h>
#include <spine/AtlasAttachmentLoader.h>
#include <spine/SkeletonBinary.h>
#include <spine/SkeletonData.h>
#include <spine/Skeleton.h>
#include <spine/SkeletonClipping.h>
#include <spine/AnimationState.h>
#include <spine/AnimationStateData.h>
#include <spine/Animation.h>
#include <spine/Slot.h>
#include <spine/SlotData.h>
#include <spine/Bone.h>
#include <spine/Attachment.h>
#include <spine/RegionAttachment.h>
#include <spine/MeshAttachment.h>
#include <spine/ColorTimeline.h>
#include <spine/ClippingAttachment.h>
#include <spine/Skin.h>
#include <spine/BlendMode.h>
#include <spine/RTTI.h>

#include "PathUtils.h"
#include <spdlog/spdlog.h>

#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace fs = std::filesystem;

namespace rt {

// ─── Deleters ───────────────────────────────────────────────────────────────
void SpineParsedSkeleton::SkelDataDeleter::operator()(spine::SkeletonData* p) const { delete p; }
void SpineEngine::SkelDeleter::operator()(spine::Skeleton* p) const { delete p; }

// ─── Shared parsed data ─────────────────────────────────────────────────────
SpineParsedSkeleton::SpineParsedSkeleton() = default;
SpineParsedSkeleton::~SpineParsedSkeleton() = default;

std::shared_ptr<SpineParsedSkeleton> SpineParsedSkeleton::parseFromBuffers(
    const std::vector<uint8_t>& skelBytes, const std::string& atlasText,
    const std::string& atlasDir, const std::string& skelPath,
    const std::string& atlasPath, float scale)
{
    auto parsed = std::make_shared<SpineParsedSkeleton>();
    if (!parsed->atlas.loadFromMemory(atlasText.data(),
                                      static_cast<int>(atlasText.size()),
                                      atlasDir)) {
        spdlog::error("SpineEngine: atlas load from memory failed");
        return nullptr;
    }

    spine::AtlasAttachmentLoader attachmentLoader(parsed->atlas.getSpineAtlas());
    spine::SkeletonBinary binary(&attachmentLoader);
    binary.setScale(scale);
    auto* data = binary.readSkeletonData(
        reinterpret_cast<const unsigned char*>(skelBytes.data()),
        static_cast<int>(skelBytes.size()));
    if (!data) {
        spdlog::error("SpineEngine: failed to load skeleton from buffer: {}",
                      binary.getError().buffer());
        return nullptr;
    }
    parsed->skelData.reset(data);
    parsed->skelPath  = skelPath;
    parsed->atlasPath = atlasPath;

    spdlog::info("SpineEngine: parsed skeleton from buffers — {} bones, {} slots, {} animations",
                 data->getBones().size(), data->getSlots().size(),
                 data->getAnimations().size());
    return parsed;
}

std::shared_ptr<SpineParsedSkeleton> SpineParsedSkeleton::parseFromFiles(
    const std::string& skelPath, const std::string& atlasPath, float scale)
{
    auto parsed = std::make_shared<SpineParsedSkeleton>();
    parsed->version = SpineEngine::detectVersion(skelPath);
    spdlog::info("SpineEngine: loading skeleton '{}' (version: {})",
                 pathToUtf8(utf8ToPath(skelPath).filename()),
                 parsed->version.empty() ? "unknown" : parsed->version);

    if (!parsed->atlas.load(atlasPath)) {
        spdlog::error("SpineEngine: atlas load failed: {}", atlasPath);
        return nullptr;
    }

    spine::AtlasAttachmentLoader attachmentLoader(parsed->atlas.getSpineAtlas());
    spine::SkeletonBinary binary(&attachmentLoader);
    binary.setScale(scale);
    auto* data = binary.readSkeletonDataFile(spine::String(skelPath.c_str()));
    if (!data) {
        spdlog::error("SpineEngine: failed to load skeleton: {} — {}",
                      skelPath, binary.getError().buffer());
        return nullptr;
    }
    parsed->skelData.reset(data);
    parsed->skelPath  = skelPath;
    parsed->atlasPath = atlasPath;

    spdlog::info("SpineEngine: loaded skeleton — {} bones, {} slots, {} animations, {} skins",
                 data->getBones().size(), data->getSlots().size(),
                 data->getAnimations().size(), data->getSkins().size());
    return parsed;
}

namespace {

/// Skeleton::getBounds' vertical extent, counting only what is drawn: a
/// slot faded to nothing still has geometry.  Modernia's outfit_02 action
/// carries a fully transparent effect far above and below her, and
/// measuring it shrank her to about 60% on the timeline.  Same cut-off as
/// extractMeshes' culling.
void extendByVisibleY(spine::Skeleton& skeleton, spine::Vector<float>& verts,
                      float& minY, float& maxY)
{
    const float skeletonAlpha = skeleton.getColor().a;
    auto& drawOrder = skeleton.getDrawOrder();
    for (size_t i = 0; i < drawOrder.size(); ++i) {
        spine::Slot* slot = drawOrder[i];
        if (!slot || !slot->getBone().isActive()) continue;
        spine::Attachment* attachment = slot->getAttachment();
        if (!attachment) continue;
        size_t count = 0;
        float attachmentAlpha = 1.0f;
        if (attachment->getRTTI().instanceOf(spine::RegionAttachment::rtti)) {
            auto* region = static_cast<spine::RegionAttachment*>(attachment);
            attachmentAlpha = region->getColor().a;
            count = 8;
            if (verts.size() < count) verts.setSize(count, 0);
            region->computeWorldVertices(*slot, verts, 0);
        } else if (attachment->getRTTI().instanceOf(spine::MeshAttachment::rtti)) {
            auto* mesh = static_cast<spine::MeshAttachment*>(attachment);
            attachmentAlpha = mesh->getColor().a;
            count = mesh->getWorldVerticesLength();
            if (verts.size() < count) verts.setSize(count, 0);
            mesh->computeWorldVertices(*slot, 0, count, verts.buffer(), 0);
        }
        if (count == 0 ||
            skeletonAlpha * slot->getColor().a * attachmentAlpha < 0.004f)
            continue;
        for (size_t v = 1; v < count; v += 2) {
            minY = std::min(minY, verts[v]);
            maxY = std::max(maxY, verts[v]);
        }
    }
}

} // namespace

uint32_t spineRenderHeight(uint32_t outH, const SpineBox& setup, const SpineBox& reach)
{
    if (setup.h <= 1.0f || reach.h <= 1.0f) return outH;
    const float centreY = setup.y + setup.h * 0.5f;
    const float halfReach = std::max(reach.y + reach.h - centreY, centreY - reach.y);
    const float growth = std::clamp(2.0f * halfReach / setup.h, 1.0f, kSpineMaxRenderGrowth);
    if (growth <= 1.0f) return outH;
    // (- 0.01: float noise must not add a row, 1080 * 1.6f is 1728.00003)
    const auto h = static_cast<uint32_t>(
        std::ceil(static_cast<float>(outH) * growth - 0.01f));
    return h + (h & 1u);
}

SpineBox SpineParsedSkeleton::framingBox(const std::string& animation) const
{
    std::lock_guard lock(m_framingMutex);
    if (auto it = m_framingBoxes.find(animation); it != m_framingBoxes.end())
        return it->second;

    SpineBox box;
    if (!skelData) return box;

    // A skeleton of its own: sampling must not inherit another animation's
    // pose (the old per-animation boxes were measured while still mixed
    // from the animation sampled before them).
    spine::Skeleton skeleton(skelData.get());
    spine::Vector<float> verts;
    skeleton.setToSetupPose();
    skeleton.updateWorldTransform();
    skeleton.getBounds(box.x, box.y, box.w, box.h, verts);

    spine::Animation* anim = animation.empty()
        ? nullptr : skelData->findAnimation(spine::String(animation.c_str()));
    if (anim && box.w > 0.0f && box.h > 0.0f) {
        // About 30 samples a second finds the extent; bounds move smoothly.
        const float dur = anim->getDuration();
        const int samples = dur > 0.0f
            ? std::clamp(static_cast<int>(dur * 30.0f), 8, 240) : 0;
        float minY = box.y;
        float maxY = box.y + box.h;
        spine::Vector<spine::Event*> events;
        for (int s = 0; s <= samples; ++s) {
            const float t = samples > 0
                ? dur * static_cast<float>(s) / static_cast<float>(samples) : 0.0f;
            skeleton.setToSetupPose();
            anim->apply(skeleton, t, t, false, &events, 1.0f,
                        spine::MixBlend_Setup, spine::MixDirection_In);
            events.clear();
            skeleton.updateWorldTransform();
            extendByVisibleY(skeleton, verts, minY, maxY);
        }
        box.y = minY;
        box.h = maxY - minY;
    }
    m_framingBoxes.emplace(animation, box);
    return box;
}

void SpineEngine::ClipperDeleter::operator()(spine::SkeletonClipping* p) const { delete p; }

// ─── Construction ───────────────────────────────────────────────────────────
SpineEngine::SpineEngine() = default;
SpineEngine::~SpineEngine() = default;

SpineEngine::SpineEngine(SpineEngine&&) noexcept = default;
SpineEngine& SpineEngine::operator=(SpineEngine&&) noexcept = default;

void SpineEngine::setHiddenAttachmentNames(std::vector<std::string> names)
{
    m_hiddenAttachmentNames.clear();
    for (auto& name : names) {
        if (!name.empty())
            m_hiddenAttachmentNames.emplace(std::move(name));
    }
}

// ─── Version detection ──────────────────────────────────────────────────────
std::string SpineEngine::detectVersion(const std::string& skelPath)
{
    std::ifstream file(skelPath, std::ios::binary);
    if (!file.is_open()) return {};

    // Read first 4KB — version string is near the start of the binary
    constexpr size_t kBufSize = 4096;
    std::vector<char> buf(kBufSize);
    file.read(buf.data(), kBufSize);
    auto bytesRead = file.gcount();

    // Look for pattern "4.X.YY" where X is 0-2 and YY is digits
    std::string content(buf.data(), static_cast<size_t>(bytesRead));
    for (size_t i = 0; i + 5 < content.size(); ++i) {
        if (content[i] == '4' && content[i + 1] == '.') {
            // Find the end of the version string
            size_t end = i + 2;
            while (end < content.size() &&
                   (std::isdigit(static_cast<unsigned char>(content[end])) || content[end] == '.')) {
                ++end;
            }
            if (end - i >= 5) {  // at least "4.X.Y"
                return content.substr(i, end - i);
            }
        }
    }
    return {};
}

// ─── Path resolution ────────────────────────────────────────────────────────
SpineEngine::ResolvedPaths SpineEngine::resolvePaths(
    const std::string& assetsDir,
    const std::string& character,
    const std::string& outfit,
    CharacterStance stance)
{
    ResolvedPaths result;

    // Build directory: assets/characters/{Character}/{outfit}/[stance/]
    fs::path baseDir = utf8ToPath(assetsDir) / "characters" / character / outfit;

    if (stance == CharacterStance::Aim) {
        baseDir = baseDir / "aim";
    } else if (stance == CharacterStance::Cover) {
        baseDir = baseDir / "cover";
    }

    if (!fs::exists(baseDir) || !fs::is_directory(baseDir)) {
        spdlog::warn("SpineEngine: directory not found: {}", pathToUtf8(baseDir));
        return result;
    }

    // Find .skel file in the directory
    for (auto& entry : fs::directory_iterator(baseDir)) {
        auto ext = pathToUtf8(entry.path().extension());
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (ext == ".skel" && result.skelPath.empty()) {
            result.skelPath = pathToUtf8(entry.path());
        } else if (ext == ".atlas" && result.atlasPath.empty()) {
            result.atlasPath = pathToUtf8(entry.path());
        } else if (ext == ".png" && result.texturePath.empty()) {
            result.texturePath = pathToUtf8(entry.path());
        }
    }

    result.valid = !result.skelPath.empty() && !result.atlasPath.empty();

    if (!result.valid) {
        spdlog::warn("SpineEngine: missing .skel or .atlas in {}", pathToUtf8(baseDir));
    }

    return result;
}

// ─── Load skeleton ──────────────────────────────────────────────────────────
bool SpineEngine::loadSkeleton(const std::string& skelPath,
                                const std::string& atlasPath,
                                float scale)
{
    // Skip if already loaded with the same files
    if (m_skeleton && m_loadedSkelPath == skelPath && m_loadedAtlasPath == atlasPath)
        return true;

    return adoptParsed(SpineParsedSkeleton::parseFromFiles(skelPath, atlasPath, scale));
}

// ─── Build per-engine state on shared parsed data ───────────────────────────
bool SpineEngine::adoptParsed(std::shared_ptr<SpineParsedSkeleton> parsed)
{
    // Reset any existing state — order matters!
    // AnimationState references Skeleton and SkeletonData, so destroy it first.
    m_animation = SpineAnimation{};
    m_skeleton.reset();
    m_clipper.reset();
    m_parsed.reset();
    m_version.clear();
    m_loadedSkelPath.clear();
    m_loadedAtlasPath.clear();

    if (!parsed || !parsed->skelData)
        return false;
    m_parsed = std::move(parsed);
    auto* data = m_parsed->skelData.get();

    m_skeleton.reset(new spine::Skeleton(data));
    m_skeleton->setToSetupPose();
    m_skeleton->updateWorldTransform();

    m_clipper.reset(new spine::SkeletonClipping());

    m_animation.init(m_skeleton.get(), data, 0.2f);
    computeAlphaAnimatedSlots();

    m_version         = m_parsed->version;
    m_loadedSkelPath  = m_parsed->skelPath;
    m_loadedAtlasPath = m_parsed->atlasPath;
    return true;
}

const SpineAtlas& SpineEngine::atlas() const noexcept
{
    static const SpineAtlas s_empty;
    return m_parsed ? m_parsed->atlas : s_empty;
}

// ─── Load from in-memory buffers ────────────────────────────────────────────
bool SpineEngine::loadSkeletonFromBuffers(const std::vector<uint8_t>& skelBytes,
                                           const std::string& atlasText,
                                           const std::string& atlasDir,
                                           const std::string& skelPath,
                                           const std::string& atlasPath,
                                           float scale)
{
    // Skip if already loaded with the same files
    if (m_skeleton && m_loadedSkelPath == skelPath && m_loadedAtlasPath == atlasPath)
        return true;

    return adoptParsed(SpineParsedSkeleton::parseFromBuffers(
        skelBytes, atlasText, atlasDir, skelPath, atlasPath, scale));
}

// ─── Load from clip ─────────────────────────────────────────────────────────
bool SpineEngine::loadFromClip(const SpineClip& clip, const std::string& assetsDir)
{
    auto paths = resolvePaths(assetsDir, clip.characterName(), clip.outfit(), clip.stance());
    if (!paths.valid) return false;

    if (!loadSkeleton(paths.skelPath, paths.atlasPath)) return false;

    if (!clip.animationName().empty()) {
        m_animation.setBodyAnimation(clip.animationName(), clip.isLooping());
    }
    m_animation.setSpeed(clip.animationSpeed());

    if (clip.isTalking()) {
        m_animation.startTalking();
    } else {
        m_animation.stopTalking();
    }

    return true;
}

// ─── Load from clip using pre-cached buffers ────────────────────────────────
bool SpineEngine::loadFromClipBuffered(const SpineClip& clip,
                                        const std::vector<uint8_t>& skelBytes,
                                        const std::string& atlasText,
                                        const std::string& atlasDir,
                                        const std::string& skelPath,
                                        const std::string& atlasPath)
{
    if (!loadSkeletonFromBuffers(skelBytes, atlasText, atlasDir, skelPath, atlasPath))
        return false;

    // Apply clip settings (same as loadFromClip)
    if (!clip.animationName().empty()) {
        m_animation.setBodyAnimation(clip.animationName(), clip.isLooping());
    }
    m_animation.setSpeed(clip.animationSpeed());

    if (clip.isTalking()) {
        m_animation.startTalking();
    } else {
        m_animation.stopTalking();
    }

    return true;
}

// ─── Load from clip using shared parsed data ────────────────────────────────
bool SpineEngine::loadFromClipParsed(const SpineClip& clip,
                                      std::shared_ptr<SpineParsedSkeleton> parsed)
{
    if (!adoptParsed(std::move(parsed)))
        return false;

    // Apply clip settings (same as loadFromClip)
    if (!clip.animationName().empty()) {
        m_animation.setBodyAnimation(clip.animationName(), clip.isLooping());
    }
    m_animation.setSpeed(clip.animationSpeed());

    if (clip.isTalking()) {
        m_animation.startTalking();
    } else {
        m_animation.stopTalking();
    }

    return true;
}

// ─── Skeleton info ──────────────────────────────────────────────────────────
float SpineEngine::skeletonWidth() const
{
    return skelData() ? skelData()->getWidth() : 0.0f;
}

float SpineEngine::skeletonHeight() const
{
    return skelData() ? skelData()->getHeight() : 0.0f;
}

bool SpineEngine::setSkin(const std::string& skinName)
{
    if (!m_skeleton) return false;

    auto* skin = skelData()->findSkin(spine::String(skinName.c_str()));
    if (!skin) {
        spdlog::warn("SpineEngine: skin '{}' not found", skinName);
        return false;
    }

    m_skeleton->setSkin(skin);
    m_skeleton->setSlotsToSetupPose();
    m_skeleton->updateWorldTransform();
    return true;
}

std::vector<std::string> SpineEngine::listSkins() const
{
    std::vector<std::string> result;
    if (!skelData()) return result;

    auto& skins = skelData()->getSkins();
    result.reserve(skins.size());
    for (size_t i = 0; i < skins.size(); ++i) {
        result.emplace_back(skins[i]->getName().buffer());
    }
    return result;
}

void SpineEngine::setPosition(float x, float y)
{
    if (m_skeleton) m_skeleton->setPosition(x, y);
}

void SpineEngine::setScale(float sx, float sy)
{
    if (m_skeleton) {
        m_skeleton->setScaleX(sx);
        m_skeleton->setScaleY(sy);
    }
}

// ─── Frame evaluation ───────────────────────────────────────────────────────
void SpineEngine::update(float dt)
{
    if (!m_skeleton) return;
    try {
        m_animation.update(dt);
        m_animation.apply();
    } catch (const std::exception& e) {
        spdlog::error("SpineEngine::update exception: {}", e.what());
    } catch (...) {
        spdlog::error("SpineEngine::update unknown exception");
    }
}

void SpineEngine::evaluateAtTime(float bodyTime, float talkTime)
{
    if (!m_skeleton) return;
    try {
        m_animation.evaluateAtTime(bodyTime, talkTime);
    } catch (const std::exception& e) {
        spdlog::error("SpineEngine::evaluateAtTime exception: {}", e.what());
    } catch (...) {
        spdlog::error("SpineEngine::evaluateAtTime unknown exception");
    }
}

// ─── Bounds ─────────────────────────────────────────────────────────────────
void SpineEngine::getBounds(float& x, float& y, float& w, float& h)
{
    if (!m_skeleton) {
        x = y = w = h = 0;
        return;
    }
    spine::Vector<float> vertBuf;
    m_skeleton->getBounds(x, y, w, h, vertBuf);
}

// ─── Mesh extraction ────────────────────────────────────────────────────────
namespace {

SpineBlendMode convertBlendMode(spine::BlendMode mode)
{
    switch (mode) {
        case spine::BlendMode_Normal:   return SpineBlendMode::Normal;
        case spine::BlendMode_Additive: return SpineBlendMode::Additive;
        case spine::BlendMode_Multiply: return SpineBlendMode::Multiply;
        case spine::BlendMode_Screen:   return SpineBlendMode::Screen;
        default:                        return SpineBlendMode::Normal;
    }
}

} // anonymous namespace

void SpineEngine::computeAlphaAnimatedSlots()
{
    m_slotAlphaAnimated.clear();
    if (!skelData()) return;
    m_slotAlphaAnimated.assign(skelData()->getSlots().size(), 0);

    auto& anims = skelData()->getAnimations();
    for (size_t a = 0; a < anims.size(); ++a) {
        auto& tls = anims[a]->getTimelines();
        for (size_t t = 0; t < tls.size(); ++t) {
            spine::Timeline* tl = tls[t];
            int slotIdx = -1;
            // Only alpha-bearing colour timelines matter (RGB/RGB2 animate
            // colour but not opacity, so those slots can still be culled).
            if (tl->getRTTI().isExactly(spine::RGBATimeline::rtti))
                slotIdx = static_cast<spine::RGBATimeline*>(tl)->getSlotIndex();
            else if (tl->getRTTI().isExactly(spine::AlphaTimeline::rtti))
                slotIdx = static_cast<spine::AlphaTimeline*>(tl)->getSlotIndex();
            else if (tl->getRTTI().isExactly(spine::RGBA2Timeline::rtti))
                slotIdx = static_cast<spine::RGBA2Timeline*>(tl)->getSlotIndex();
            if (slotIdx >= 0 && static_cast<size_t>(slotIdx) < m_slotAlphaAnimated.size())
                m_slotAlphaAnimated[slotIdx] = 1;
        }
    }
}

SpineRenderData SpineEngine::extractMeshes()
{
    SpineRenderData result;
    if (!m_skeleton || !m_clipper) return result;

    try {

    auto& drawOrder = m_skeleton->getDrawOrder();
    spine::Vector<float> worldVertices;

    // Temporary storage for a region attachment's 8 floats (4 vertices * 2 components)
    float regionVerts[8];

    for (size_t i = 0; i < drawOrder.size(); ++i) {
        auto* slot = drawOrder[i];
        if (!slot) {
            continue;
        }

        // Skip invisible slots (alpha ~0) to save meshing/upload/draw — BUT only
        // for slots whose alpha is NOT animated.  A slot that an animation fades
        // through ~0 (e.g. Modernia's R_acc_light glow at its trough) must stay
        // in the mesh: if it pops in/out as alpha crosses the threshold, the
        // changing vertex/index count produces a one-frame "black" render glitch
        // on the timeline GPU path (only there; COMPOSE's drifting update(dt)
        // rarely samples the exact trough frame).  Such a slot renders invisibly
        // anyway (premultiplied-alpha → zero contribution), so keeping it is free
        // of visual cost.  Statically-hidden slots are still culled.
        const int slotIdx = slot->getData().getIndex();
        const bool alphaAnimated = slotIdx >= 0 &&
            static_cast<size_t>(slotIdx) < m_slotAlphaAnimated.size() &&
            m_slotAlphaAnimated[slotIdx];
        if (!alphaAnimated && slot->getColor().a < 0.004f) {
            m_clipper->clipEnd(*slot);
            continue;
        }

        auto* attachment = slot->getAttachment();
        if (!attachment) {
            m_clipper->clipEnd(*slot);
            continue;
        }
        const char* attachmentName = attachment->getName().buffer();
        if (attachmentName && m_hiddenAttachmentNames.count(attachmentName) != 0) {
            m_clipper->clipEnd(*slot);
            continue;
        }

        float* vertices = nullptr;
        size_t vertexCount = 0;
        float* uvs = nullptr;
        unsigned short* triangles = nullptr;
        size_t triangleCount = 0;
        int pageIndex = -1;
        spine::Color attachColor;

        // Check attachment type using RTTI
        if (attachment->getRTTI().isExactly(spine::RegionAttachment::rtti)) {
            auto* region = static_cast<spine::RegionAttachment*>(attachment);
            region->computeWorldVertices(*slot, regionVerts, 0, 2);

            vertices = regionVerts;
            vertexCount = 4;
            uvs = region->getUVs().buffer();

            // Region uses 2 triangles (0,1,2 and 2,3,0)
            static unsigned short regionIndices[] = { 0, 1, 2, 2, 3, 0 };
            triangles = regionIndices;
            triangleCount = 6;

            attachColor = region->getColor();

            // Find texture page from atlas region.
            // getRegion() returns the AtlasRegion* stored by setRegion()
            // during skeleton loading (upcast to TextureRegion*).
            auto* texRegion = region->getRegion();
            if (texRegion) {
                auto* atlasRegion = static_cast<spine::AtlasRegion*>(texRegion);
                if (atlasRegion->page) {
                    pageIndex = atlasRegion->page->index;
                }
            }
        }
        else if (attachment->getRTTI().isExactly(spine::MeshAttachment::rtti)) {
            auto* mesh = static_cast<spine::MeshAttachment*>(attachment);

            worldVertices.setSize(mesh->getWorldVerticesLength(), 0);
            mesh->computeWorldVertices(*slot, 0, mesh->getWorldVerticesLength(),
                                       worldVertices.buffer(), 0, 2);

            vertices = worldVertices.buffer();
            vertexCount = mesh->getWorldVerticesLength() / 2;
            uvs = mesh->getUVs().buffer();
            triangles = mesh->getTriangles().buffer();
            triangleCount = mesh->getTriangles().size();

            attachColor = mesh->getColor();

            auto* texRegion = mesh->getRegion();
            if (texRegion) {
                auto* atlasRegion = static_cast<spine::AtlasRegion*>(texRegion);
                if (atlasRegion->page) {
                    pageIndex = atlasRegion->page->index;
                }
            }
        }
        else if (attachment->getRTTI().isExactly(spine::ClippingAttachment::rtti)) {
            auto* clip = static_cast<spine::ClippingAttachment*>(attachment);
            m_clipper->clipStart(*slot, clip);
            continue;
        }
        else {
            m_clipper->clipEnd(*slot);
            continue;
        }

        if (!vertices || vertexCount == 0 || !triangles || triangleCount == 0) {
            m_clipper->clipEnd(*slot);
            continue;
        }

        // Atlas texels are PMA, so the vertex tint must also be premultiplied
        // by the combined skeleton/slot/attachment alpha. Otherwise a faded
        // slot keeps full-strength RGB and still contributes through the
        // renderer's src=ONE blend state, even when its alpha reaches zero.
        auto& skelColor = m_skeleton->getColor();
        auto& slotColor = slot->getColor();

        float a = skelColor.a * slotColor.a * attachColor.a;
        float r = skelColor.r * slotColor.r * attachColor.r * a;
        float g = skelColor.g * slotColor.g * attachColor.g * a;
        float b = skelColor.b * slotColor.b * attachColor.b * a;

        auto blendMode = convertBlendMode(slot->getData().getBlendMode());

        // Apply clipping if active
        if (m_clipper->isClipping()) {
            m_clipper->clipTriangles(vertices, triangles, triangleCount, uvs, 2);

            auto& clippedVerts = m_clipper->getClippedVertices();
            auto& clippedTris  = m_clipper->getClippedTriangles();
            auto& clippedUVs   = m_clipper->getClippedUVs();

            if (clippedTris.size() == 0) {
                m_clipper->clipEnd(*slot);
                continue;
            }

            // Only merge with the LAST batch if it matches page+blend;
            // never search earlier batches — that would break draw order.
            SpineRenderBatch* batch = nullptr;
            if (!result.batches.empty()) {
                auto& last = result.batches.back();
                if (last.texturePageIndex == pageIndex && last.blendMode == blendMode)
                    batch = &last;
            }
            if (!batch) {
                result.batches.emplace_back();
                batch = &result.batches.back();
                batch->texturePageIndex = pageIndex;
                batch->blendMode = blendMode;
                const char* sn = slot->getData().getName().buffer();
                batch->debugFirstSlot = sn ? sn : "";
            }

            auto baseVertex = static_cast<uint16_t>(batch->vertices.size());
            size_t clippedVertCount = clippedVerts.size() / 2;
            for (size_t vi = 0; vi < clippedVertCount; ++vi) {
                SpineVertex sv;
                sv.x = clippedVerts[vi * 2];
                sv.y = clippedVerts[vi * 2 + 1];
                sv.u = clippedUVs[vi * 2];
                sv.v = clippedUVs[vi * 2 + 1];
                sv.r = r; sv.g = g; sv.b = b; sv.a = a;
                batch->vertices.push_back(sv);
            }
            for (size_t ti = 0; ti < clippedTris.size(); ++ti) {
                batch->indices.push_back(baseVertex + clippedTris[ti]);
            }
        }
        else {
            // No clipping — add directly.
            // Only merge with the LAST batch to preserve draw order.
            SpineRenderBatch* batch = nullptr;
            if (!result.batches.empty()) {
                auto& last = result.batches.back();
                if (last.texturePageIndex == pageIndex && last.blendMode == blendMode)
                    batch = &last;
            }
            if (!batch) {
                result.batches.emplace_back();
                batch = &result.batches.back();
                batch->texturePageIndex = pageIndex;
                batch->blendMode = blendMode;
                const char* sn = slot->getData().getName().buffer();
                batch->debugFirstSlot = sn ? sn : "";
            }

            auto baseVertex = static_cast<uint16_t>(batch->vertices.size());
            for (size_t vi = 0; vi < vertexCount; ++vi) {
                SpineVertex sv;
                sv.x = vertices[vi * 2];
                sv.y = vertices[vi * 2 + 1];
                sv.u = uvs[vi * 2];
                sv.v = uvs[vi * 2 + 1];
                sv.r = r; sv.g = g; sv.b = b; sv.a = a;
                batch->vertices.push_back(sv);
            }
            for (size_t ti = 0; ti < triangleCount; ++ti) {
                batch->indices.push_back(baseVertex + triangles[ti]);
            }
        }

        m_clipper->clipEnd(*slot);
    }

    m_clipper->clipEnd();

    if (m_skeleton) {
        spine::Vector<float> vertBuf;
        m_skeleton->getBounds(result.boundsX, result.boundsY,
                              result.boundsW, result.boundsH, vertBuf);
    }

    } catch (const std::exception& e) {
        spdlog::error("SpineEngine::extractMeshes exception: {}", e.what());
    } catch (...) {
        spdlog::error("SpineEngine::extractMeshes unknown exception");
    }

    return result;
}

} // namespace rt

#endif // ROUNDTABLE_HAS_SPINE
