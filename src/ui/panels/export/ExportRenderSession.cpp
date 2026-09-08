#include "panels/export/ExportRenderSession.h"

#include "CompositeService.h"
#include "PathUtils.h"
#include "cache/CachePolicy.h"
#include "cache/FrameCache.h"
#include "playback/MediaPool.h"
#include "project/Project.h"
#include "timeline/Clip.h"
#include "timeline/ImageClip.h"
#include "timeline/PngPuppetClip.h"
#include "timeline/SequenceClip.h"
#include "timeline/SpineClip.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/VideoClip.h"

#ifdef ROUNDTABLE_HAS_SPINE
#include "spine/ModelManager.h"
#endif

#include <QImage>
#include <QString>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>

namespace rt {

ExportRenderSession::ExportRenderSession(Dependencies dependencies)
    : m_dependencies(std::move(dependencies))
{}

ExportRenderSession::~ExportRenderSession() = default;

void ExportRenderSession::ensureInitialized()
{
    if (m_compositeService)
        return;

    m_cachePolicy = std::make_unique<CachePolicy>();
    m_compositeService = std::make_unique<CompositeService>(
        CompositeService::GpuResourceMode::Isolated);
    m_compositeService->setMediaPool(m_dependencies.mediaPool);
    m_compositeService->setMediaSourceService(
        m_dependencies.mediaSourceService);
    m_compositeService->setModelManager(m_dependencies.modelManager);
    m_compositeService->setShotPresetManager(
        m_dependencies.shotPresetManager);
    m_compositeService->setCachePolicy(m_cachePolicy.get());
    m_compositeService->setSegmentCacheReadEnabled(true);
#ifdef ROUNDTABLE_HAS_SPINE
    if (m_dependencies.mediaPool)
        m_compositeService->initAnimVideoCache(m_dependencies.mediaPool);
#endif
}

void ExportRenderSession::bindSnapshot(
    const std::shared_ptr<const RenderSnapshot>& snapshot)
{
    ensureInitialized();
    if (!snapshot || !snapshot->project || !snapshot->timeline)
        return;
    if (m_projectSnapshot.get() == snapshot->project.get() &&
        m_timelineSnapshot.get() == snapshot->timeline.get())
        return;

    m_compositeService->reset();
    m_projectSnapshot = snapshot->project;
    m_timelineSnapshot = snapshot->timeline;
    // CompositeService's historical binding API is non-const; the export
    // graph itself remains strongly owned and read-only for the whole bind.
    m_compositeService->setProject(
        const_cast<Project*>(m_projectSnapshot.get()));
    m_compositeService->setTimeline(
        const_cast<Timeline*>(m_timelineSnapshot.get()));
#ifdef ROUNDTABLE_HAS_SPINE
    if (m_dependencies.mediaPool)
        m_compositeService->initAnimVideoCache(m_dependencies.mediaPool);
#endif
}

RenderPreflightResult ExportRenderSession::preflight(
    const std::shared_ptr<const RenderSnapshot>& snapshot)
{
    RenderPreflightResult result;
    if (!snapshot || !snapshot->project || !snapshot->timeline) {
        result.status = RenderResultStatus::Failed;
        result.warning = "export resource preflight requires a complete snapshot";
        return result;
    }

    bindSnapshot(snapshot);

    if (m_preflightSnapshot != snapshot.get() ||
        m_preflightVersion != snapshot->editVersion) {
        m_preflightSnapshot = snapshot.get();
        m_preflightVersion = snapshot->editVersion;
        m_mediaOpenAttempts.clear();
        m_puppetStates.clear();
    }

    size_t missingCount = 0;
    size_t failedCount = 0;
    std::vector<std::string> warnings;
    std::unordered_set<std::string> resources;
    std::unordered_set<const Timeline*> visiting;

    auto addWarning = [&warnings](std::string warning) {
        if (!warning.empty() && warnings.size() < 4)
            warnings.push_back(std::move(warning));
    };
    auto record = [&](const std::string& key, ResourceLoadState state,
                      std::string warning) {
        if (!resources.insert(key).second) return;
        ++result.totalResources;
        switch (state) {
        case ResourceLoadState::Ready:
            ++result.readyResources;
            break;
        case ResourceLoadState::Unresolved:
        case ResourceLoadState::Loading:
            ++result.pendingResources;
            addWarning(std::move(warning));
            break;
        case ResourceLoadState::Missing:
            ++missingCount;
            addWarning(std::move(warning));
            break;
        case ResourceLoadState::Failed:
            ++failedCount;
            addWarning(std::move(warning));
            break;
        }
    };

    auto checkMedia = [&](const std::string& path) {
        const std::string key = "media:" + path;
        if (resources.count(key)) return;
        if (path.empty()) {
            record(key, ResourceLoadState::Missing,
                   "A video or image clip has no media path");
            return;
        }
        if (!m_dependencies.mediaPool) {
            record(key, ResourceLoadState::Failed,
                   "Media loader is unavailable: " + path);
            return;
        }
        const auto fsPath = utf8ToPath(path);
        ResourceLoadState state =
            m_dependencies.mediaPool->pathState(fsPath);
        if (state == ResourceLoadState::Unresolved) {
            if (m_mediaOpenAttempts.insert(path).second) {
                m_dependencies.mediaPool->openAsync(fsPath);
                state = ResourceLoadState::Loading;
            } else {
                state = ResourceLoadState::Failed;
            }
        }
        const std::string warning =
            state == ResourceLoadState::Missing
                ? "Media is offline: " + path
                : (state == ResourceLoadState::Failed
                       ? "Media could not be opened: " + path
                       : (state == ResourceLoadState::Loading
                              ? "Opening media: " + path : std::string{}));
        record(key, state, warning);
    };

    auto checkPuppet = [&](const PngPuppetClip& puppet) {
        const std::string& path = puppet.facePath(
            PngPuppetClip::MouthClosedEyesOpen);
        const std::string key = "puppet:" +
            (path.empty() ? std::to_string(puppet.id()) : path);
        if (resources.count(key)) return;

        ResourceLoadState state = ResourceLoadState::Missing;
        auto cached = m_puppetStates.find(key);
        if (cached != m_puppetStates.end()) {
            state = cached->second;
        } else if (!path.empty()) {
            std::error_code ec;
            if (std::filesystem::exists(utf8ToPath(path), ec) && !ec) {
                const QImage image(QString::fromStdString(path));
                state = image.isNull() ? ResourceLoadState::Failed
                                       : ResourceLoadState::Ready;
            }
            m_puppetStates.emplace(key, state);
        }
        const std::string warning =
            path.empty()
                ? "PNG puppet has no resting face image: " +
                      puppet.characterName()
                : (state == ResourceLoadState::Missing
                       ? "PNG puppet image is offline: " + path
                       : (state == ResourceLoadState::Failed
                              ? "PNG puppet image could not be decoded: " + path
                              : std::string{}));
        record(key, state, warning);
    };

    auto checkSpine = [&](const SpineClip& spine) {
        const int stance = static_cast<int>(spine.stance());
        const std::string identity = spine.characterName() + "|" +
            spine.outfit() + "|" + std::to_string(stance);
        const std::string key = "spine:" + identity;
        if (resources.count(key)) return;
        if (spine.characterName().empty()) {
            record(key, ResourceLoadState::Missing,
                   "Spine clip has no character identity");
            return;
        }
#ifdef ROUNDTABLE_HAS_SPINE
        auto shared = m_compositeService->findSpineSharedData(identity);
        if (!shared) {
            shared = m_compositeService->getOrCreateSharedSpineData(
                spine, m_dependencies.assetsDir);
        }
        if (!shared) {
            record(key, ResourceLoadState::Failed,
                   "Spine loader is unavailable: " + identity);
            return;
        }
        record(key, shared->loadState,
               shared->loadWarning.empty()
                   ? (shared->loadState == ResourceLoadState::Loading
                          ? "Loading Spine character: " + identity
                          : std::string{})
                   : shared->loadWarning);
#else
        record(key, ResourceLoadState::Failed,
               "This build cannot render Spine character: " + identity);
#endif
    };

    const Project& project = *snapshot->project;
    std::function<void(const Timeline*, int64_t, int64_t)> visitTimeline;
    visitTimeline = [&](const Timeline* timeline,
                        int64_t rangeStart, int64_t rangeEnd) {
        if (!timeline || failedCount > 0) return;
        if (!visiting.insert(timeline).second) {
            ++failedCount;
            addWarning("Nested sequence cycle detected during export preflight");
            return;
        }
        for (size_t ti = 0; ti < timeline->trackCount(); ++ti) {
            const Track* track = timeline->track(ti);
            if (!track || track->type() != TrackType::Video || track->isMuted())
                continue;
            for (size_t ci = 0; ci < track->clipCount(); ++ci) {
                const Clip* clip = track->clip(ci);
                if (!clip || !clip->isEnabled()) continue;
                if (clip->timelineOut() <= rangeStart ||
                    clip->timelineIn() >= rangeEnd)
                    continue;
                if (const auto* video = dynamic_cast<const VideoClip*>(clip))
                    checkMedia(video->mediaPath());
                else if (const auto* image = dynamic_cast<const ImageClip*>(clip))
                    checkMedia(image->mediaPath());
                else if (const auto* puppet =
                             dynamic_cast<const PngPuppetClip*>(clip))
                    checkPuppet(*puppet);
                else if (const auto* spine = dynamic_cast<const SpineClip*>(clip))
                    checkSpine(*spine);
                else if (const auto* nested =
                             dynamic_cast<const SequenceClip*>(clip)) {
                    if (nested->sequenceIndex() >= project.sequenceCount()) {
                        ++failedCount;
                        addWarning("Nested sequence index is invalid: " +
                                   std::to_string(nested->sequenceIndex()));
                    } else {
                        const int64_t overlapStart = std::max(
                            rangeStart, nested->timelineIn());
                        const int64_t overlapEnd = std::min(
                            rangeEnd, nested->timelineOut());
                        const int64_t nestedStart = nested->sourceIn() +
                            overlapStart - nested->timelineIn();
                        const int64_t nestedEnd = nested->sourceIn() +
                            overlapEnd - nested->timelineIn();
                        visitTimeline(project.sequence(nested->sequenceIndex()),
                                      nestedStart, nestedEnd);
                    }
                }
            }
        }
        visiting.erase(timeline);
    };
    visitTimeline(snapshot->timeline.get(),
                  snapshot->rangeStartTick.value_or(
                      std::numeric_limits<int64_t>::lowest()),
                  snapshot->rangeEndTick.value_or(
                      std::numeric_limits<int64_t>::max()));

    if (failedCount > 0)
        result.status = RenderResultStatus::Failed;
    else if (missingCount > 0)
        result.status = RenderResultStatus::MissingMedia;
    else if (result.pendingResources > 0)
        result.status = RenderResultStatus::Pending;
    else
        result.status = RenderResultStatus::Ready;

    if (!warnings.empty()) {
        std::ostringstream message;
        for (size_t i = 0; i < warnings.size(); ++i) {
            if (i) message << "; ";
            message << warnings[i];
        }
        result.warning = message.str();
    }
    return result;
}

RenderResult ExportRenderSession::renderFrame(
    const std::shared_ptr<const RenderSnapshot>& snapshot,
    int64_t tick, uint32_t outW, uint32_t outH,
    bool scrubMode, bool preserveAlpha)
{
    RenderResult result;
    result.timelineTick = tick;
    if (!snapshot || !snapshot->project || !snapshot->timeline ||
        outW == 0 || outH == 0) {
        result.status = RenderResultStatus::Failed;
        result.diagnostics.status = result.status;
        result.diagnostics.warning =
            "export render requires a complete snapshot and non-zero dimensions";
        return result;
    }

    bindSnapshot(snapshot);

    result.frame = m_compositeService->tryBuild16fPassthrough(tick, outW, outH);
    if (result.frame) {
        result.status = RenderResultStatus::Ready;
        result.diagnostics.status = result.status;
    } else {
        RenderRequest request;
        request.type = RenderRequestType::Export;
        request.quality = RenderQuality::Full;
        request.exactness = RenderExactness::ExactRequired;
        request.timelineTick = tick;
        request.outputWidth = outW;
        request.outputHeight = outH;
        request.snapshot = snapshot;
        request.scrubMode = scrubMode;
        request.stillFrame = true;
        request.preferGpuOutput = false;
        request.forceFullResolution = true;
        request.preserveAlpha = preserveAlpha;
        request.caller = "ExportRenderSession::renderFrame";
        result = m_compositeService->renderFrame(request);
    }
    if (result.frame)
        result.frame->preservesAlpha = preserveAlpha;
    return result;
}

bool ExportRenderSession::storeFrame(
    const std::shared_ptr<const RenderSnapshot>& snapshot,
    int64_t tick, const std::shared_ptr<CachedFrame>& frame)
{
    if (!m_compositeService || !snapshot || !frame ||
        m_projectSnapshot.get() != snapshot->project.get() ||
        m_timelineSnapshot.get() != snapshot->timeline.get()) {
        return false;
    }
    m_compositeService->cacheExportFrame(tick, frame);
    return true;
}

} // namespace rt
