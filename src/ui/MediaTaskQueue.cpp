#include "MediaTaskQueue.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace rt {

namespace {

size_t defaultWorkerCount()
{
    const unsigned hardware = std::thread::hardware_concurrency();
    if (hardware <= 2) return 1;
    return std::min<size_t>(4, std::max<size_t>(2, hardware / 2));
}

} // namespace

struct MediaTaskQueue::State
{
    struct Subscriber {
        OwnerId owner{0};
        ErasedCompletion completion;
        std::atomic<bool> canceled{false};
    };

    struct Task {
        std::string key;
        Priority priority{Priority::Background};
        uint64_t sequence{0};
        ErasedWork work;
        std::vector<std::shared_ptr<Subscriber>> subscribers;
        std::stop_source stopSource;
        bool running{false};
    };

    mutable std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::shared_ptr<Task>> pending;
    std::unordered_map<std::string, std::shared_ptr<Task>> byKey;
    std::vector<std::jthread> workers;
    std::atomic<OwnerId> nextOwner{1};
    uint64_t nextSequence{1};
    bool stopping{false};
    Statistics stats;
};

MediaTaskQueue::MediaTaskQueue(size_t workerCount)
    : m_state(std::make_unique<State>())
{
    if (workerCount == 0) workerCount = defaultWorkerCount();
    workerCount = std::max<size_t>(1, workerCount);
    m_state->stats.workers = workerCount;
    m_state->workers.reserve(workerCount);
    for (size_t i = 0; i < workerCount; ++i) {
        m_state->workers.emplace_back(
            [this](std::stop_token stop) { workerLoop(stop); });
    }
}

MediaTaskQueue::~MediaTaskQueue()
{
    shutdown();
}

MediaTaskQueue& MediaTaskQueue::instance()
{
    static MediaTaskQueue queue;
    return queue;
}

MediaTaskQueue::OwnerId MediaTaskQueue::createOwner()
{
    return m_state->nextOwner.fetch_add(1, std::memory_order_relaxed);
}

bool MediaTaskQueue::submitErased(OwnerId owner,
                                  std::string key,
                                  Priority priority,
                                  ErasedWork work,
                                  ErasedCompletion completion)
{
    if (owner == 0 || key.empty() || !work || !completion) return false;

    auto subscriber = std::make_shared<State::Subscriber>();
    subscriber->owner = owner;
    subscriber->completion = std::move(completion);

    {
        std::lock_guard lock(m_state->mutex);
        if (m_state->stopping) return false;

        auto found = m_state->byKey.find(key);
        if (found != m_state->byKey.end()) {
            const auto& task = found->second;
            const bool usable = task && !task->stopSource.stop_requested();
            if (usable) {
                const auto duplicateOwner = std::find_if(
                    task->subscribers.begin(), task->subscribers.end(),
                    [owner](const auto& current) {
                        return current->owner == owner &&
                               !current->canceled.load(std::memory_order_acquire);
                    });
                if (duplicateOwner == task->subscribers.end()) {
                    task->subscribers.push_back(std::move(subscriber));
                }
                if (!task->running && priority > task->priority)
                    task->priority = priority;
                ++m_state->stats.deduplicated;
                return true;
            }
        }

        auto task = std::make_shared<State::Task>();
        task->key = std::move(key);
        task->priority = priority;
        task->sequence = m_state->nextSequence++;
        task->work = std::move(work);
        task->subscribers.push_back(std::move(subscriber));
        m_state->byKey[task->key] = task;
        m_state->pending.push_back(task);
        ++m_state->stats.submitted;
        m_state->stats.queued = m_state->pending.size();
    }
    m_state->ready.notify_one();
    return true;
}

void MediaTaskQueue::promote(const std::string& key, Priority priority)
{
    std::lock_guard lock(m_state->mutex);
    const auto found = m_state->byKey.find(key);
    if (found != m_state->byKey.end() && found->second &&
        !found->second->running && priority > found->second->priority) {
        found->second->priority = priority;
    }
}

void MediaTaskQueue::cancelOwner(OwnerId owner)
{
    if (owner == 0) return;

    std::lock_guard lock(m_state->mutex);
    for (auto it = m_state->byKey.begin(); it != m_state->byKey.end(); ) {
        const auto& task = it->second;
        bool anyActive = false;
        for (const auto& subscriber : task->subscribers) {
            if (subscriber->owner == owner &&
                !subscriber->canceled.exchange(true, std::memory_order_acq_rel)) {
                ++m_state->stats.canceled;
            }
            if (!subscriber->canceled.load(std::memory_order_acquire))
                anyActive = true;
        }

        if (!anyActive) {
            task->stopSource.request_stop();
            if (!task->running) {
                std::erase(m_state->pending, task);
                it = m_state->byKey.erase(it);
                continue;
            }
        }
        ++it;
    }
    m_state->stats.queued = m_state->pending.size();
}

void MediaTaskQueue::shutdown()
{
    if (!m_state) return;

    {
        std::lock_guard lock(m_state->mutex);
        if (m_state->stopping && m_state->workers.empty()) return;
        m_state->stopping = true;
        for (auto& [key, task] : m_state->byKey) {
            (void)key;
            task->stopSource.request_stop();
            for (const auto& subscriber : task->subscribers)
                subscriber->canceled.store(true, std::memory_order_release);
        }
        m_state->pending.clear();
        m_state->stats.queued = 0;
    }

    for (auto& worker : m_state->workers) worker.request_stop();
    m_state->ready.notify_all();
    m_state->workers.clear(); // jthread destructors join

    std::lock_guard lock(m_state->mutex);
    m_state->byKey.clear();
    m_state->stats.running = 0;
}

MediaTaskQueue::Statistics MediaTaskQueue::statistics() const
{
    std::lock_guard lock(m_state->mutex);
    Statistics result = m_state->stats;
    result.queued = m_state->pending.size();
    result.workers = m_state->workers.size();
    return result;
}

void MediaTaskQueue::workerLoop(std::stop_token workerStop)
{
    while (!workerStop.stop_requested()) {
        std::shared_ptr<State::Task> task;
        {
            std::unique_lock lock(m_state->mutex);
            m_state->ready.wait(lock, [this, &workerStop] {
                return m_state->stopping || workerStop.stop_requested() ||
                       !m_state->pending.empty();
            });
            if (m_state->stopping || workerStop.stop_requested()) return;

            const auto best = std::max_element(
                m_state->pending.begin(), m_state->pending.end(),
                [](const auto& lhs, const auto& rhs) {
                    if (lhs->priority != rhs->priority)
                        return lhs->priority < rhs->priority;
                    return lhs->sequence > rhs->sequence;
                });
            task = *best;
            m_state->pending.erase(best);
            task->running = true;
            m_state->stats.queued = m_state->pending.size();
            ++m_state->stats.running;
        }

        ErasedResult result;
        if (!task->stopSource.stop_requested()) {
            try {
                result = task->work(task->stopSource.get_token());
            } catch (const std::exception& error) {
                result.error = error.what();
            } catch (...) {
                result.error = "unknown exception";
            }
        }

        std::vector<std::shared_ptr<State::Subscriber>> subscribers;
        {
            std::lock_guard lock(m_state->mutex);
            task->running = false;
            if (m_state->stats.running > 0) --m_state->stats.running;

            const auto found = m_state->byKey.find(task->key);
            if (found != m_state->byKey.end() && found->second == task)
                m_state->byKey.erase(found);

            if (!m_state->stopping && !task->stopSource.stop_requested()) {
                subscribers = task->subscribers;
                if (result.error.empty() && result.value)
                    ++m_state->stats.completed;
                else
                    ++m_state->stats.failed;
            }
        }

        if (!result.error.empty() && !task->stopSource.stop_requested())
            spdlog::warn("Media task '{}' failed: {}", task->key, result.error);

        for (const auto& subscriber : subscribers) {
            if (subscriber->canceled.load(std::memory_order_acquire)) continue;
            try {
                subscriber->completion(result);
            } catch (const std::exception& error) {
                spdlog::error("Media task '{}' completion failed: {}",
                              task->key, error.what());
            } catch (...) {
                spdlog::error("Media task '{}' completion failed with an unknown exception",
                              task->key);
            }
        }
    }
}

} // namespace rt
