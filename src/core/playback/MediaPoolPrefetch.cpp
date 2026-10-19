// MediaPoolPrefetch.cpp - Background prefetch coordinator.
//
// Thin coordinator after extracting:
//   loop pre-decode    → MediaPoolPrefetchLoop.cpp
//   schedule + worker  → MediaPoolPrefetchSchedule.cpp
//   frame conversion   → MediaPoolPrefetchConvert.cpp
//
// Contains: startPrefetchThread(), stopPrefetchThread(),
// startLoopPreDecodeWorkers(), stopLoopPreDecodeWorkers().

#include "MediaPool.h"
#include "MediaPoolPrefetchInternal.h"

#include <spdlog/spdlog.h>

namespace rt {

void MediaPool::startPrefetchThread()
{
    if (m_prefetchRunning) return;
    m_prefetchRunning = true;
    const int threadCount = perfProfile().prefetchThreadCount;
    m_prefetchThreads.reserve(threadCount);
    for (int i = 0; i < threadCount; ++i) {
        WorkerThread worker;
        worker.start("MediaPool prefetch " + std::to_string(i + 1),
                     [this, i](std::stop_token) { prefetchWorker(i); });
        m_prefetchThreads.push_back(std::move(worker));
    }
    spdlog::info("MediaPool: {} prefetch worker threads started", threadCount);
    startLoopPreDecodeWorkers();
}

void MediaPool::stopPrefetchThread()
{
    {
        std::lock_guard lock(m_prefetchMutex);
        m_prefetchRunning = false;
        m_prefetchQueue.clear();
        m_prefetchPackedOwner.clear();
    }
    m_prefetchCv.notify_all();
    for (auto& t : m_prefetchThreads)
        t.stop();
    m_prefetchThreads.clear();

    stopLoopPreDecodeWorkers();
    {
        std::lock_guard lock(m_loopPreDecodeMutex);
        m_loopPreDecodeActive.clear();
        m_loopPreDecodeDone.clear();
        std::priority_queue<LoopPreDecodeTask> empty;
        std::swap(m_loopPreDecodeQueue, empty);
    }

    spdlog::info("MediaPool: prefetch worker threads stopped");
}

void MediaPool::startLoopPreDecodeWorkers()
{
    if (!m_loopPreDecodeThreads.empty()) return;
    m_loopPreDecodeThreads.reserve(LOOP_PREDECODE_MAX_CONCURRENT);
    for (int i = 0; i < LOOP_PREDECODE_MAX_CONCURRENT; ++i) {
        WorkerThread worker;
        worker.start("MediaPool loop pre-decode " + std::to_string(i + 1),
                     [this](std::stop_token stop) { loopPreDecodeDispatcher(stop); });
        m_loopPreDecodeThreads.push_back(std::move(worker));
    }
    spdlog::info("MediaPool: {} loop pre-decode worker threads started",
                 LOOP_PREDECODE_MAX_CONCURRENT);
}

void MediaPool::stopLoopPreDecodeWorkers()
{
    // Ask all of them first so they wind down together, then join each.
    for (auto& worker : m_loopPreDecodeThreads)
        worker.requestStop();
    for (auto& worker : m_loopPreDecodeThreads)
        worker.stop();
    m_loopPreDecodeThreads.clear();
}

// ═════════════════════════════════════════════════════════════════════════════

} // namespace rt
