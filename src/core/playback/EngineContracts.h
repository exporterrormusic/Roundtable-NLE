#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rt {

class Project;
class Timeline;
struct CachedFrame;

enum class RenderRequestType : uint8_t
{
    Playback,
    Scrub,
    Still,
    Thumbnail,
    SourceMonitor,
    Export
};

enum class RenderQuality : uint8_t
{
    Auto,
    Full,
    Half,
    Quarter,
    Proxy,
    Native
};

enum class RenderExactness : uint8_t
{
    BestEffortAllowed,
    ExactRequired
};

enum class RenderNodeKind : uint8_t
{
    Media,
    Audio,
    Character,
    Graphic,
    Title,
    Caption,
    Adjustment,
    NestedSequence,
    Transition,
    Mask,
    Generated
};

enum class RenderResultStatus : uint8_t
{
    Ready,
    Blank,
    Pending,
    HeldPrevious,
    Failed,
    MissingMedia,
    Canceled
};

/// Authoritative state of an external resource owned by a loader. Renderers
/// must not infer permanence from a null payload: Unresolved/Loading are
/// retryable, Missing is a confirmed absent dependency, and Failed means the
/// dependency was found but could not be parsed or rendered.
enum class ResourceLoadState : uint8_t
{
    Unresolved,
    Loading,
    Ready,
    Missing,
    Failed
};

enum class RenderResourceKind : uint8_t
{
    MediaFrame,
    AudioSamples,
    CharacterPose,
    GraphicRaster,
    TitleRaster,
    NestedSequence,
    Transition,
    Mask,
    Effect
};

struct MediaSourceHandle
{
    uint64_t id{0};
};

struct CharacterSourceHandle
{
    uint64_t id{0};
};

/// Immutable model graph selected for a render request.
///
/// Live preview requests may leave this null and use the render service's
/// explicitly-bound live project. Export and other asynchronous consumers
/// should provide a snapshot so a request cannot silently render a different
/// project after the editor switches sequences.
struct RenderSnapshot
{
    std::shared_ptr<const Project>  project;
    std::shared_ptr<const Timeline> timeline;
    size_t                          sequenceIndex{0};
    uint64_t                        editVersion{0};
    /// Optional half-open timeline interval required by the consumer. Export
    /// fills this from its captured frame range so preflight does not reject
    /// an offline clip that lies wholly outside the requested output.
    std::optional<int64_t>          rangeStartTick;
    std::optional<int64_t>          rangeEndTick;

    [[nodiscard]] bool hasTimeline() const noexcept { return timeline != nullptr; }
    [[nodiscard]] bool isFullProject() const noexcept {
        return project != nullptr && timeline != nullptr;
    }
};

struct RenderRequest
{
    using Clock = std::chrono::steady_clock;

    uint64_t requestId{0};
    RenderRequestType type{RenderRequestType::Playback};
    RenderQuality quality{RenderQuality::Auto};
    RenderExactness exactness{RenderExactness::BestEffortAllowed};
    int64_t timelineTick{0};
    uint32_t outputWidth{0};
    uint32_t outputHeight{0};
    Clock::time_point deadline{};
    std::shared_ptr<const RenderSnapshot> snapshot;
    /// Optional live-project sequence selection. Snapshot-backed requests use
    /// snapshot->timeline instead; ordinary Program Monitor requests leave
    /// this unset and use the service's bound timeline.
    std::optional<size_t> targetSequenceIndex;

    // Per-request render policy. These replace call-site mode toggles while
    // the legacy compositor API is migrated incrementally.
    bool scrubMode{false};
    bool stillFrame{false};
    bool preferGpuOutput{false};
    bool forceFullResolution{false};
    bool preserveAlpha{false};

    std::string caller;

    /// True only for a normal clock-driven Program Monitor pass. Paused
    /// exact frames, scrub, export, and forced-quality work have different
    /// latency contracts and must not train real-time playback diagnostics.
    [[nodiscard]] bool isRealtimePlaybackPass() const noexcept
    {
        return type == RenderRequestType::Playback &&
               !scrubMode && !stillFrame && !forceFullResolution;
    }
};

struct RenderNode
{
    uint64_t stableId{0};
    uint64_t sourceId{0};
    uint64_t peerSourceId{0};
    uint64_t groupId{0};
    RenderNodeKind kind{RenderNodeKind::Media};
    int64_t timelineIn{0};
    int64_t duration{0};
    int64_t localTick{0};
    int64_t sourceIn{0};
    int64_t sourceOut{0};
    int64_t sourceTick{0};
    int trackIndex{0};
    int stackOrder{0};
    int32_t blendMode{0};
    int32_t transitionType{-1};
    float transitionProgress{-1.0f};
    float opacity{1.0f};
    float positionX{0.0f};
    float positionY{0.0f};
    float scaleX{1.0f};
    float scaleY{1.0f};
    float rotation{0.0f};
    float cropLeft{0.0f};
    float cropRight{0.0f};
    float cropTop{0.0f};
    float cropBottom{0.0f};
    float clipVolume{1.0f};
    float clipPan{0.0f};
    float trackVolume{1.0f};
    float trackPan{0.0f};
    double playbackSpeed{1.0};
    double effectiveSpeed{1.0};
    uint32_t sourceWidth{0};
    uint32_t sourceHeight{0};
    size_t effectCount{0};
    size_t maskCount{0};
    size_t graphicLayerCount{0};
    bool enabled{true};
    bool trackMuted{false};
    bool maintainPitch{true};
    bool hasActiveEffects{false};
    bool hasMasks{false};
    bool audioNeeded{false};
    bool looping{false};
    bool stillSource{false};
    bool characterSource{false};
    std::string label;
    std::string shotName;
    std::string layerId;
    std::string sourcePath;
};

struct TimelineSnapshot
{
    uint64_t editVersion{0};
    int64_t evaluationTick{0};
    int64_t sequenceDuration{0};
    double frameRate{0.0};
    std::vector<RenderNode> nodes;
};

struct RenderGraphNode
{
    uint64_t nodeId{0};
    RenderNodeKind kind{RenderNodeKind::Media};
    int trackIndex{0};
    int stackOrder{0};
    size_t firstResource{0};
    size_t resourceCount{0};
};

struct RenderResourceRequest
{
    RenderResourceKind kind{RenderResourceKind::MediaFrame};
    uint64_t nodeId{0};
    uint64_t sourceId{0};
    uint64_t peerSourceId{0};
    int64_t sourceTick{0};
    int64_t duration{0};
    RenderRequestType requestType{RenderRequestType::Playback};
    RenderQuality quality{RenderQuality::Auto};
    RenderExactness exactness{RenderExactness::BestEffortAllowed};
    uint32_t sourceWidth{0};
    uint32_t sourceHeight{0};
    size_t effectCount{0};
    size_t maskCount{0};
    size_t graphicLayerCount{0};
    bool audioNeeded{false};
    bool stillSource{false};
    bool characterSource{false};
    std::string sourcePath;
};

struct RenderGraph
{
    uint64_t editVersion{0};
    int64_t evaluationTick{0};
    double frameRate{0.0};
    std::vector<RenderGraphNode> nodes;
    std::vector<RenderResourceRequest> resources;
};

struct FrameDiagnostics
{
    uint64_t requestId{0};
    RenderResultStatus status{RenderResultStatus::Pending};
    // CPU wall-clock stages for one real-time preview request.
    double cacheLookupMs{0.0};
    double prewarmMs{0.0};
    double shotBoundaryMs{0.0};
    double timelineEvalMs{0.0};
    double mediaResolveMs{0.0};
    double gpuRecordSubmitMs{0.0};
    double readbackMs{0.0};
    double renderMs{0.0};
    double presentMs{0.0};
    double producerMs{0.0};
    // Resolved Vulkan timestamps lag the current request by the submission
    // ring depth, but their aggregate accurately identifies GPU saturation.
    double gpuFrameMs{0.0};
    double gpuUploadMs{0.0};
    double gpuEffectMs{0.0};
    double gpuCompositeMs{0.0};
    uint64_t uploadBytes{0};
    uint32_t layerCount{0};
    bool cacheHit{false};
    bool compositeCacheHit{false};
    bool segmentCacheHit{false};
    bool gpuTimingsValid{false};
    bool heldFrame{false};
    bool droppedFrame{false};
    std::string warning;
};

/// The outcome of one render request.  A frame pointer alone cannot express
/// whether a renderer intentionally produced an empty timeline, temporarily
/// missed a resource, held an older preview frame, or failed permanently.
/// Keeping that state beside the payload also gives asynchronous boundaries a
/// stable place to carry request provenance and diagnostics.
struct RenderResult
{
    RenderResultStatus status{RenderResultStatus::Pending};
    std::shared_ptr<CachedFrame> frame;
    FrameDiagnostics diagnostics;
    int64_t timelineTick{0};

    [[nodiscard]] bool isComplete() const noexcept
    {
        return status == RenderResultStatus::Ready ||
               status == RenderResultStatus::Blank ||
               status == RenderResultStatus::HeldPrevious;
    }

    [[nodiscard]] bool isRetryable() const noexcept
    {
        return status == RenderResultStatus::Pending;
    }
};

/// Aggregate readiness of the external visual resources required by an
/// immutable render snapshot.  Export uses this before opening an encoder so
/// cold asynchronous loaders can finish (or report a terminal problem)
/// without turning the first few frame requests into timing-dependent retries.
struct RenderPreflightResult
{
    RenderResultStatus status{RenderResultStatus::Pending};
    size_t totalResources{0};
    size_t readyResources{0};
    size_t pendingResources{0};
    std::string warning;

    [[nodiscard]] bool isReady() const noexcept
    {
        return status == RenderResultStatus::Ready;
    }

    [[nodiscard]] bool isRetryable() const noexcept
    {
        return status == RenderResultStatus::Pending;
    }
};

[[nodiscard]] inline const char* toString(RenderRequestType type) noexcept
{
    switch (type) {
    case RenderRequestType::Playback:      return "Playback";
    case RenderRequestType::Scrub:         return "Scrub";
    case RenderRequestType::Still:         return "Still";
    case RenderRequestType::Thumbnail:     return "Thumbnail";
    case RenderRequestType::SourceMonitor: return "SourceMonitor";
    case RenderRequestType::Export:        return "Export";
    }
    return "Unknown";
}

[[nodiscard]] inline const char* toString(RenderQuality quality) noexcept
{
    switch (quality) {
    case RenderQuality::Auto:    return "Auto";
    case RenderQuality::Full:    return "Full";
    case RenderQuality::Half:    return "Half";
    case RenderQuality::Quarter: return "Quarter";
    case RenderQuality::Proxy:   return "Proxy";
    case RenderQuality::Native:  return "Native";
    }
    return "Unknown";
}

[[nodiscard]] inline const char* toString(RenderExactness exactness) noexcept
{
    switch (exactness) {
    case RenderExactness::BestEffortAllowed: return "BestEffortAllowed";
    case RenderExactness::ExactRequired:     return "ExactRequired";
    }
    return "Unknown";
}

[[nodiscard]] inline const char* toString(RenderResultStatus status) noexcept
{
    switch (status) {
    case RenderResultStatus::Ready:        return "Ready";
    case RenderResultStatus::Blank:        return "Blank";
    case RenderResultStatus::Pending:      return "Pending";
    case RenderResultStatus::HeldPrevious: return "HeldPrevious";
    case RenderResultStatus::Failed:       return "Failed";
    case RenderResultStatus::MissingMedia: return "MissingMedia";
    case RenderResultStatus::Canceled:     return "Canceled";
    }
    return "Unknown";
}

[[nodiscard]] inline const char* toString(RenderResourceKind kind) noexcept
{
    switch (kind) {
    case RenderResourceKind::MediaFrame:     return "MediaFrame";
    case RenderResourceKind::AudioSamples:   return "AudioSamples";
    case RenderResourceKind::CharacterPose:  return "CharacterPose";
    case RenderResourceKind::GraphicRaster:  return "GraphicRaster";
    case RenderResourceKind::TitleRaster:    return "TitleRaster";
    case RenderResourceKind::NestedSequence: return "NestedSequence";
    case RenderResourceKind::Transition:     return "Transition";
    case RenderResourceKind::Mask:           return "Mask";
    case RenderResourceKind::Effect:         return "Effect";
    }
    return "Unknown";
}

[[nodiscard]] inline RenderExactness defaultExactnessFor(RenderRequestType type) noexcept
{
    switch (type) {
    case RenderRequestType::Playback: return RenderExactness::BestEffortAllowed;
    case RenderRequestType::Scrub:
    case RenderRequestType::Still:
    case RenderRequestType::Thumbnail:
    case RenderRequestType::SourceMonitor:
    case RenderRequestType::Export:   return RenderExactness::ExactRequired;
    }
    return RenderExactness::BestEffortAllowed;
}

[[nodiscard]] inline RenderQuality defaultQualityFor(RenderRequestType type) noexcept
{
    switch (type) {
    case RenderRequestType::Playback:      return RenderQuality::Auto;
    case RenderRequestType::Scrub:         return RenderQuality::Auto;
    case RenderRequestType::Still:         return RenderQuality::Auto;
    case RenderRequestType::Thumbnail:     return RenderQuality::Quarter;
    case RenderRequestType::SourceMonitor: return RenderQuality::Half;
    case RenderRequestType::Export:        return RenderQuality::Full;
    }
    return RenderQuality::Auto;
}

} // namespace rt
