#include "panels/export/ExportRenderExecutor.h"
#include "panels/export/ExportRenderSession.h"

#include "RenderQueue.h"
#include "cache/FrameCache.h"

#include <QCoreApplication>
#include <QEvent>
#include <QMetaObject>
#include <QThread>

#include <chrono>
#include <exception>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>

namespace rt {
namespace {

RenderPreflightResult canceledPreflight()
{
    RenderPreflightResult result;
    result.status = RenderResultStatus::Canceled;
    result.warning = "export render executor is stopping";
    return result;
}

RenderResult terminalResult(RenderResultStatus status, int64_t tick,
                            std::string warning)
{
    RenderResult result;
    result.timelineTick = tick;
    result.status = status;
    result.diagnostics.status = status;
    result.diagnostics.warning = std::move(warning);
    return result;
}

} // namespace

ExportRenderExecutor::ExportRenderExecutor(QObject* parent)
    : QObject(parent)
    , m_ownerThread(QThread::currentThread())
    , m_renderThread(new QThread(this))
    , m_dispatchTarget(new QObject())
{
    m_renderThread->setObjectName(QStringLiteral("ExportRenderThread"));
}

ExportRenderExecutor::~ExportRenderExecutor()
{
    requestStop();
    stopThreadAndReleaseSession();
    clearPipeline();
    delete m_dispatchTarget;
    m_dispatchTarget = nullptr;
}

void ExportRenderExecutor::beginRun(
    std::shared_ptr<ExportRenderSession> session)
{
    Q_ASSERT(QThread::currentThread() == thread());
    Q_ASSERT(thread() == m_ownerThread);
    if (m_renderThread->isRunning())
        stopThreadAndReleaseSession();

    if (QCoreApplication::instance())
        QCoreApplication::removePostedEvents(m_dispatchTarget,
                                             QEvent::MetaCall);
    clearPipeline();
    m_stopping.store(false, std::memory_order_release);

    if (m_dispatchTarget->thread() != m_renderThread)
        m_dispatchTarget->moveToThread(m_renderThread);
    m_renderThread->start();

    const bool initialized = QMetaObject::invokeMethod(
        m_dispatchTarget,
        [this, session = std::move(session)]() mutable {
            m_session = std::move(session);
        },
        Qt::BlockingQueuedConnection);
    if (!initialized) {
        m_stopping.store(true, std::memory_order_release);
        stopThreadAndReleaseSession();
    }
}

void ExportRenderExecutor::requestStop() noexcept
{
    m_stopping.store(true, std::memory_order_release);
}

void ExportRenderExecutor::discardPendingWork()
{
    Q_ASSERT(QThread::currentThread() == thread());
    requestStop();
    if (QCoreApplication::instance())
        QCoreApplication::removePostedEvents(m_dispatchTarget,
                                             QEvent::MetaCall);
    clearPipeline();
    stopThreadAndReleaseSession();
}

QThread* ExportRenderExecutor::executionThread() const noexcept
{
    return m_renderThread;
}

void ExportRenderExecutor::stopThreadAndReleaseSession()
{
    if (!m_renderThread || !m_dispatchTarget)
        return;

    if (!m_renderThread->isRunning()) {
        m_session.reset();
        return;
    }

    // Queue work has already joined before normal cleanup reaches here, so
    // this call cannot wait behind a worker that itself needs the UI thread.
    // Destroy the render-owned session on the same thread that created its GPU
    // resources, then return the dispatch object to the UI thread for reuse.
    const bool released = QMetaObject::invokeMethod(
        m_dispatchTarget,
        [this]() {
            m_session.reset();
            m_dispatchTarget->moveToThread(m_ownerThread);
        },
        Qt::BlockingQueuedConnection);
    m_renderThread->quit();
    m_renderThread->wait();

    // Defensive fallback for an event dispatcher that stopped unexpectedly.
    if (!released) {
        m_session.reset();
        delete m_dispatchTarget;
        m_dispatchTarget = new QObject();
    }
}

void ExportRenderExecutor::clearPipeline()
{
    m_slots[0] = CompositeSlot{};
    m_slots[1] = CompositeSlot{};
    m_currentSlot = 0;
}

RenderPreflightResult ExportRenderExecutor::preflight(
    const std::shared_ptr<const ExportRenderSnapshot>& snapshot,
    const PreflightCallback& callback)
{
    RenderPreflightResult unavailable;
    unavailable.status = RenderResultStatus::Failed;
    unavailable.warning = "export resource preflight callback is unavailable";
    if (!snapshot)
        return unavailable;
    if (m_stopping.load(std::memory_order_acquire))
        return canceledPreflight();

    auto promise = std::make_shared<std::promise<RenderPreflightResult>>();
    auto future = promise->get_future();
    const bool submitted = QMetaObject::invokeMethod(
        m_dispatchTarget,
        [this, promise, callback, snapshot]() {
            if (m_stopping.load(std::memory_order_acquire)) {
                try { promise->set_value(canceledPreflight()); } catch (...) {}
                return;
            }
            try {
                if (m_session)
                    promise->set_value(m_session->preflight(snapshot));
                else if (callback)
                    promise->set_value(callback(snapshot));
                else {
                    RenderPreflightResult failed;
                    failed.status = RenderResultStatus::Failed;
                    failed.warning =
                        "export resource preflight callback is unavailable";
                    promise->set_value(std::move(failed));
                }
            } catch (...) {
                RenderPreflightResult failed;
                failed.status = RenderResultStatus::Failed;
                failed.warning =
                    "exception during export resource preflight";
                try { promise->set_value(std::move(failed)); } catch (...) {}
            }
        },
        Qt::QueuedConnection);
    if (!submitted) {
        unavailable.warning = "could not queue export resource preflight";
        return unavailable;
    }

    while (future.wait_for(std::chrono::milliseconds(20)) !=
           std::future_status::ready) {
        if (m_stopping.load(std::memory_order_acquire))
            return canceledPreflight();
    }
    try {
        return future.get();
    } catch (...) {
        unavailable.warning = "invalid export resource preflight result";
        return unavailable;
    }
}

RenderResult ExportRenderExecutor::waitForComposite(
    const std::shared_future<RenderResult>& future,
    int64_t expectedTick) const
{
    while (future.valid() &&
           future.wait_for(std::chrono::milliseconds(20)) !=
               std::future_status::ready) {
        if (m_stopping.load(std::memory_order_acquire)) {
            return terminalResult(RenderResultStatus::Canceled, expectedTick,
                                  "export render executor is stopping");
        }
    }
    if (future.valid()) return future.get();
    return terminalResult(RenderResultStatus::Failed, expectedTick,
                          "invalid export composite future");
}

std::shared_future<RenderResult> ExportRenderExecutor::submit(
    const std::shared_ptr<const ExportRenderSnapshot>& snapshot,
    int64_t tick, int slotIndex,
    uint32_t width, uint32_t height,
    bool scrubMode, bool preserveAlpha,
    const RenderCallback& callback)
{
    auto promise = std::make_shared<std::promise<RenderResult>>();
    auto future = promise->get_future().share();
    m_slots[slotIndex].snapshot = snapshot;
    m_slots[slotIndex].tick = tick;
    m_slots[slotIndex].future = future;

    if (!snapshot) {
        promise->set_value(terminalResult(
            RenderResultStatus::Failed, tick,
            "export compositor callback is unavailable"));
        return future;
    }

    const bool submitted = QMetaObject::invokeMethod(
        m_dispatchTarget,
        [this, promise, callback, snapshot, tick, width, height,
         scrubMode, preserveAlpha]() {
            if (m_stopping.load(std::memory_order_acquire)) {
                try {
                    promise->set_value(terminalResult(
                        RenderResultStatus::Canceled, tick,
                        "export render executor is stopping"));
                } catch (...) {}
                return;
            }

            // This TU is compiled with /EHa on MSVC. catch(...) therefore
            // contains external SEH raised by an injected overlay or Vulkan
            // ICD during a TDR and always resolves the worker's promise.
            try {
                RenderResult result;
                if (m_session) {
                    result = m_session->renderFrame(
                        snapshot, tick, width, height,
                        scrubMode, preserveAlpha);
                } else if (callback) {
                    result = callback(snapshot, tick, width, height,
                                      scrubMode, preserveAlpha);
                } else {
                    result = terminalResult(
                        RenderResultStatus::Failed, tick,
                        "export compositor callback is unavailable");
                }
                if (result.frame) result.frame->ensurePixels();
                promise->set_value(std::move(result));
            } catch (...) {
                spdlog::error(
                    "ExportRenderExecutor: exception during composite at tick={}",
                    tick);
                try {
                    promise->set_value(terminalResult(
                        RenderResultStatus::Failed, tick,
                        "exception during export composite"));
                } catch (...) {}
            }
        },
        Qt::QueuedConnection);
    if (!submitted) {
        try {
            promise->set_value(terminalResult(
                RenderResultStatus::Failed, tick,
                "could not queue export composite"));
        } catch (...) {}
    }
    return future;
}

RenderResult ExportRenderExecutor::render(
    const std::shared_ptr<const ExportRenderSnapshot>& snapshot,
    int64_t tick, int64_t nextTick,
    uint32_t width, uint32_t height,
    bool scrubMode, bool preserveAlpha,
    const RenderCallback& callback)
{
    if (m_stopping.load(std::memory_order_acquire)) {
        return terminalResult(RenderResultStatus::Canceled, tick,
                              "export render executor is stopping");
    }

    // The previous call pre-submitted this tick. A snapshot/tick mismatch is
    // the first frame of a run/job or a bounded retry and must render anew.
    const int current = m_currentSlot;
    const int previous = (current + 1) % 2;
    const bool firstCall =
        m_slots[previous].snapshot.get() != snapshot.get() ||
        m_slots[previous].tick != tick ||
        !m_slots[previous].future.valid();

    RenderResult result;
    result.timelineTick = tick;
    if (!firstCall && m_slots[previous].future.valid()) {
        try {
            result = waitForComposite(m_slots[previous].future, tick);
        } catch (const std::exception& error) {
            spdlog::error("ExportRenderExecutor: pipeline wait exception: {}",
                          error.what());
            result = terminalResult(RenderResultStatus::Failed, tick,
                                    "exception waiting for export composite");
        }
        m_slots[previous] = CompositeSlot{};
    }

    if (firstCall) {
        const auto first = submit(snapshot, tick, current, width, height,
                                  scrubMode, preserveAlpha, callback);
        m_currentSlot = (current + 1) % 2;
        try {
            result = waitForComposite(first, tick);
        } catch (const std::exception& error) {
            spdlog::error(
                "ExportRenderExecutor: first-frame wait exception: {}",
                error.what());
            result = terminalResult(RenderResultStatus::Failed, tick,
                                    "exception waiting for first export frame");
        }
        m_slots[current] = CompositeSlot{};

        if (nextTick >= 0) {
            (void)submit(snapshot, nextTick, m_currentSlot, width, height,
                         scrubMode, preserveAlpha, callback);
            m_currentSlot = (m_currentSlot + 1) % 2;
        }
    } else if (nextTick >= 0) {
        (void)submit(snapshot, nextTick, current, width, height,
                     scrubMode, preserveAlpha, callback);
        m_currentSlot = (current + 1) % 2;
    } else {
        m_currentSlot = current;
    }

    if (result.status != RenderResultStatus::Canceled &&
        (!result.frame || result.frame->pixels.empty())) {
        spdlog::warn(
            "ExportRenderExecutor: pipeline returned empty pixels at tick={}",
            tick);
    }
    return result;
}

bool ExportRenderExecutor::storeFrame(
    const std::shared_ptr<const ExportRenderSnapshot>& snapshot,
    int64_t tick, const std::shared_ptr<CachedFrame>& frame,
    const StoreCallback& callback)
{
    if (!snapshot || !frame)
        return false;
    if (m_stopping.load(std::memory_order_acquire))
        return false;
    auto promise = std::make_shared<std::promise<bool>>();
    auto future = promise->get_future();
    const bool submitted = QMetaObject::invokeMethod(
        m_dispatchTarget,
        [this, promise, snapshot, tick, frame, callback]() {
            if (m_stopping.load(std::memory_order_acquire)) {
                try { promise->set_value(false); } catch (...) {}
                return;
            }
            try {
                if (m_session)
                    promise->set_value(
                        m_session->storeFrame(snapshot, tick, frame));
                else if (callback) {
                    callback(snapshot, tick, frame);
                    promise->set_value(true);
                } else
                    promise->set_value(false);
            } catch (...) {
                spdlog::error(
                    "ExportRenderExecutor: exception storing frame at tick={}",
                    tick);
                try { promise->set_value(false); } catch (...) {}
            }
        },
        Qt::QueuedConnection);
    if (!submitted)
        return false;

    while (future.wait_for(std::chrono::milliseconds(20)) !=
           std::future_status::ready) {
        if (m_stopping.load(std::memory_order_acquire))
            return false;
    }
    try {
        return future.get();
    } catch (...) {
        return false;
    }
}

} // namespace rt
