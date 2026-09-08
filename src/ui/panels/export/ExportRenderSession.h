#pragma once

#include "playback/EngineContracts.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace rt {

class CachePolicy;
class CompositeService;
class MediaPool;
class MediaSourceService;
class ModelManager;
class ShotPresetManager;
struct CachedFrame;

/// Owns every mutable object and snapshot reference used by one export-queue
/// run. Construction only captures stable dependencies; mutable compositor and
/// cache state are created lazily on the dedicated export-render thread.
class ExportRenderSession final
{
public:
    struct Dependencies
    {
        MediaPool* mediaPool{nullptr};
        MediaSourceService* mediaSourceService{nullptr};
        ModelManager* modelManager{nullptr};
        ShotPresetManager* shotPresetManager{nullptr};
        std::string assetsDir{"assets"};
    };

    explicit ExportRenderSession(Dependencies dependencies);
    ~ExportRenderSession();

    ExportRenderSession(const ExportRenderSession&) = delete;
    ExportRenderSession& operator=(const ExportRenderSession&) = delete;

    [[nodiscard]] RenderPreflightResult preflight(
        const std::shared_ptr<const RenderSnapshot>& snapshot);
    [[nodiscard]] RenderResult renderFrame(
        const std::shared_ptr<const RenderSnapshot>& snapshot,
        int64_t tick, uint32_t outW, uint32_t outH,
        bool scrubMode, bool preserveAlpha);

    /// Cache a frame only when it belongs to the graph currently bound by
    /// renderFrame. Returns false rather than rebinding from the worker thread.
    [[nodiscard]] bool storeFrame(
        const std::shared_ptr<const RenderSnapshot>& snapshot,
        int64_t tick, const std::shared_ptr<CachedFrame>& frame);

private:
    void ensureInitialized();
    void bindSnapshot(const std::shared_ptr<const RenderSnapshot>& snapshot);

    Dependencies m_dependencies;
    std::unique_ptr<CachePolicy> m_cachePolicy;
    std::unique_ptr<CompositeService> m_compositeService;
    std::shared_ptr<const Project> m_projectSnapshot;
    std::shared_ptr<const Timeline> m_timelineSnapshot;

    const RenderSnapshot* m_preflightSnapshot{nullptr};
    uint64_t m_preflightVersion{0};
    std::unordered_set<std::string> m_mediaOpenAttempts;
    std::unordered_map<std::string, ResourceLoadState> m_puppetStates;
};

} // namespace rt
