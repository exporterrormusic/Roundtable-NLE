#pragma once

#include "playback/EngineContracts.h"

#include <QObject>

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>

class QThread;

namespace rt {

class ExportRenderSession;
struct CachedFrame;
struct ExportRenderSnapshot;

/// Serializes export preflight/compositing onto a dedicated render thread while
/// the RenderQueue worker encodes the previously completed frame. The executor
/// owns that thread and the run's ExportRenderSession; the panel remains a UI
/// coordinator only.
class ExportRenderExecutor final : public QObject
{
public:
    using PreflightCallback = std::function<RenderPreflightResult(
        const std::shared_ptr<const ExportRenderSnapshot>&)>;
    using RenderCallback = std::function<RenderResult(
        const std::shared_ptr<const ExportRenderSnapshot>&,
        int64_t, uint32_t, uint32_t, bool, bool)>;
    using StoreCallback = std::function<void(
        const std::shared_ptr<const ExportRenderSnapshot>&,
        int64_t, const std::shared_ptr<CachedFrame>&)>;

    explicit ExportRenderExecutor(QObject* parent = nullptr);
    ~ExportRenderExecutor() override;

    ExportRenderExecutor(const ExportRenderExecutor&) = delete;
    ExportRenderExecutor& operator=(const ExportRenderExecutor&) = delete;

    /// Called on the UI thread before a queue worker starts. Starts the render
    /// thread and transfers the run's isolated session to it.
    void beginRun(std::shared_ptr<ExportRenderSession> session = {});

    /// May be called while the worker is waiting. Waits wake within their
    /// bounded polling interval and return Canceled.
    void requestStop() noexcept;

    /// Called on the UI thread after the queue worker has joined. Removes
    /// unexecuted render calls, destroys the session on the render thread, and
    /// joins that thread.
    void discardPendingWork();

    /// Thread on which preflight, compositing, and frame storage execute.
    [[nodiscard]] QThread* executionThread() const noexcept;

    /// Called by the RenderQueue worker. The callback itself runs on the
    /// dedicated execution thread.
    [[nodiscard]] RenderPreflightResult preflight(
        const std::shared_ptr<const ExportRenderSnapshot>& snapshot,
        const PreflightCallback& callback);

    /// Called by the RenderQueue worker. Maintains the one-frame-ahead
    /// composite/encode overlap while callbacks run on the dedicated execution
    /// thread.
    [[nodiscard]] RenderResult render(
        const std::shared_ptr<const ExportRenderSnapshot>& snapshot,
        int64_t tick, int64_t nextTick,
        uint32_t width, uint32_t height,
        bool scrubMode, bool preserveAlpha,
        const RenderCallback& callback);

    /// Called by the RenderQueue worker after a frame is encoded. Serialization
    /// with compositing keeps the session-owned cache single-threaded.
    [[nodiscard]] bool storeFrame(
        const std::shared_ptr<const ExportRenderSnapshot>& snapshot,
        int64_t tick, const std::shared_ptr<CachedFrame>& frame,
        const StoreCallback& callback);

private:
    struct CompositeSlot
    {
        std::shared_future<RenderResult> future;
        std::shared_ptr<const ExportRenderSnapshot> snapshot;
        int64_t tick{-1};
    };

    [[nodiscard]] RenderResult waitForComposite(
        const std::shared_future<RenderResult>& future,
        int64_t expectedTick) const;
    [[nodiscard]] std::shared_future<RenderResult> submit(
        const std::shared_ptr<const ExportRenderSnapshot>& snapshot,
        int64_t tick, int slotIndex,
        uint32_t width, uint32_t height,
        bool scrubMode, bool preserveAlpha,
        const RenderCallback& callback);
    void clearPipeline();
    void stopThreadAndReleaseSession();

    std::atomic<bool> m_stopping{true};
    CompositeSlot m_slots[2];
    int m_currentSlot{0};
    QThread* m_ownerThread{nullptr};
    QThread* m_renderThread{nullptr};
    QObject* m_dispatchTarget{nullptr};
    std::shared_ptr<ExportRenderSession> m_session;
};

} // namespace rt
