/*
 * TimelineWorkspaceSpine.cpp  -- Spine UI integration for TimelineWorkspace.
 *
 * Heavy spine logic (rendering, shared-data creation, per-clip engine
 * management) lives in CompositeServiceSpine.cpp.  This file contains
 * only the Qt-dependent scheduling entry-point and thin wrappers that
 * forward to the CompositeService.
 */

#include "panels/timeline/TimelineWorkspace.h"
#include "PathUtils.h"
#include "panels/monitors/ProgramMonitor.h"

#include "CompositeService.h"

#include "timeline/SpineClip.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"

#ifdef ROUNDTABLE_HAS_SPINE
#include "spine/SpineEngine.h"
#include "stb_image.h"
#endif

#include <QMetaObject>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>

namespace rt {

#ifdef ROUNDTABLE_HAS_SPINE

// ---- Background spine shared data loading --------------------------------
// Runs the expensive disk I/O + atlas decode on a background thread,
// then posts the result back to the UI thread via QMetaObject::invokeMethod
// so the compositor picks it up on the next refresh.
void TimelineWorkspace::scheduleSpineSharedLoad(
    const std::string& charName, const std::string& outfit,
    int stance, const std::string& assetsDir)
{
    if (!m_compositeService) return;

    // Build the cache key (character|outfit|stance).
    std::string key = charName + "|" + outfit + "|" + std::to_string(stance);

    // Already cached?
    if (m_compositeService->findSpineSharedData(key))
        return;

    // Already being loaded?  addSpinePendingKey returns false if already pending.
    if (!m_compositeService->addSpinePendingKey(key))
        return;

    spdlog::info("scheduleSpineSharedLoad: background loading '{}'", key);

    // Heavy work on a background thread.
    auto future = std::async(std::launch::async,
        [this, key, charName, outfit, stance, assetsDir]() {
            auto shared = std::make_shared<CompositeService::SpineSharedData>();

            // Resolve skeleton/atlas file paths
            auto paths = SpineEngine::resolvePaths(
                assetsDir, charName, outfit,
                static_cast<CharacterStance>(stance));
            if (!paths.valid) {
                spdlog::warn("scheduleSpineSharedLoad: failed to resolve '{}'", key);
                QMetaObject::invokeMethod(this, [this, key, shared]() {
                    m_compositeService->integrateSpineSharedData(key, shared);
                    m_compositeService->removeSpinePendingKey(key);
                });
                return;
            }
            shared->skelPath  = paths.skelPath;
            shared->atlasPath = paths.atlasPath;

            // Read file contents into memory
            {
                std::ifstream skelFile(paths.skelPath, std::ios::binary | std::ios::ate);
                if (skelFile.is_open()) {
                    auto sz = skelFile.tellg();
                    skelFile.seekg(0);
                    shared->skelBytes.resize(static_cast<size_t>(sz));
                    skelFile.read(reinterpret_cast<char*>(shared->skelBytes.data()), sz);
                }
                std::ifstream atlasFile(paths.atlasPath, std::ios::binary | std::ios::ate);
                if (atlasFile.is_open()) {
                    auto sz = atlasFile.tellg();
                    atlasFile.seekg(0);
                    shared->atlasText.resize(static_cast<size_t>(sz));
                    atlasFile.read(shared->atlasText.data(), sz);
                    shared->atlasDir = pathToUtf8(std::filesystem::path(paths.atlasPath).parent_path());
                }
            }

            // Load temporary engine for atlas info + bounds
            SpineEngine tempEngine;
            if (!tempEngine.loadSkeleton(paths.skelPath, paths.atlasPath)) {
                spdlog::warn("scheduleSpineSharedLoad: skeleton load failed for '{}'", key);
                QMetaObject::invokeMethod(this, [this, key, shared]() {
                    m_compositeService->integrateSpineSharedData(key, shared);
                    m_compositeService->removeSpinePendingKey(key);
                });
                return;
            }

            // Decode atlas page PNGs (the expensive part)
            const auto& pages = tempEngine.atlas().pages();
            const auto& atlasDir = tempEngine.atlas().directory();
            shared->pagePixels.resize(pages.size());
            shared->pageWidths.resize(pages.size(), 0);
            shared->pageHeights.resize(pages.size(), 0);
            shared->pagePMA.resize(pages.size(), false);

            for (size_t pi = 0; pi < pages.size(); ++pi) {
                std::string texPath = atlasDir + "/" + pages[pi].texturePath;
                shared->pagePMA[pi] = pages[pi].pma;
                int w = 0, h = 0, ch = 0;
                uint8_t* pixels = stbi_load(texPath.c_str(), &w, &h, &ch, 4);
                if (pixels) {
                    shared->pagePixels[pi].assign(pixels, pixels + w * h * 4);
                    shared->pageWidths[pi] = w;
                    shared->pageHeights[pi] = h;
                    stbi_image_free(pixels);
                }
            }
            shared->pagePixelsUnpremultiplied = false;

            // Pre-cache bounds
            tempEngine.getBounds(
                shared->stableBoundsX, shared->stableBoundsY,
                shared->stableBoundsW, shared->stableBoundsH);
            shared->boundsCached = true;

            spdlog::info("scheduleSpineSharedLoad: loaded '{}' ({} atlas pages)",
                         key, pages.size());

            // Merge result into main-thread cache and trigger refresh
            QMetaObject::invokeMethod(this, [this, key, shared]() {
                m_compositeService->integrateSpineSharedData(key, shared);
                m_compositeService->removeSpinePendingKey(key);
                invalidateCompositeCache();
                if (m_programMonitor) m_programMonitor->requestRefresh();
            });
        });

    // Clean up completed futures to avoid unbounded growth
    m_compositeService->drainCompletedSpineFutures();
    m_compositeService->addSpineFuture(std::move(future));
}

// ---- Thin wrappers -------------------------------------------------------
void TimelineWorkspace::warmNewSpineClips()
{
    if (m_compositeService) m_compositeService->warmNewSpineClips();
}

void TimelineWorkspace::preloadSpineAssets()
{
    if (m_compositeService) m_compositeService->preloadSpineAssets();
}

#endif // ROUNDTABLE_HAS_SPINE

} // namespace rt