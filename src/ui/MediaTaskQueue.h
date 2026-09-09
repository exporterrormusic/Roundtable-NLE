/*
 * MediaTaskQueue -- bounded, cancellable background work for UI media jobs.
 *
 * The queue owns a small, fixed set of workers. Requests with the same key
 * share one decode and fan the result out to every interested owner.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <utility>

namespace rt {

class MediaTaskQueue final
{
public:
    using OwnerId = uint64_t;

    enum class Priority : uint8_t {
        Background = 0,
        Visible = 1,
        Interactive = 2,
    };

    template <typename T>
    struct Result {
        std::shared_ptr<const T> value;
        std::string error;

        [[nodiscard]] bool succeeded() const noexcept
        {
            return value != nullptr && error.empty();
        }

        static Result success(T result)
        {
            return {std::make_shared<T>(std::move(result)), {}};
        }

        static Result failure(std::string message)
        {
            return {nullptr, std::move(message)};
        }
    };

    struct Statistics {
        uint64_t submitted{0};
        uint64_t deduplicated{0};
        uint64_t completed{0};
        uint64_t failed{0};
        uint64_t canceled{0};
        size_t queued{0};
        size_t running{0};
        size_t workers{0};
    };

    explicit MediaTaskQueue(size_t workerCount = 0);
    ~MediaTaskQueue();

    MediaTaskQueue(const MediaTaskQueue&) = delete;
    MediaTaskQueue& operator=(const MediaTaskQueue&) = delete;

    static MediaTaskQueue& instance();

    [[nodiscard]] OwnerId createOwner();

    template <typename T, typename Work, typename Completion>
    bool submit(OwnerId owner,
                std::string key,
                Priority priority,
                Work&& work,
                Completion&& completion)
    {
        ErasedWork erasedWork =
            [fn = std::forward<Work>(work)](std::stop_token stop) mutable {
                Result<T> typed = fn(stop);
                return ErasedResult{std::move(typed.value), std::move(typed.error)};
            };

        ErasedCompletion erasedCompletion =
            [fn = std::forward<Completion>(completion)](ErasedResult result) mutable {
                Result<T> typed;
                typed.value = std::static_pointer_cast<const T>(std::move(result.value));
                typed.error = std::move(result.error);
                fn(std::move(typed));
            };

        return submitErased(owner, std::move(key), priority,
                            std::move(erasedWork), std::move(erasedCompletion));
    }

    /// Raise the priority of queued work without adding another subscriber.
    void promote(const std::string& key, Priority priority);

    /// Remove an owner's queued callbacks and request cancellation when no
    /// other owner still needs the shared task.
    void cancelOwner(OwnerId owner);

    /// Stop accepting work, request cancellation, and join every worker.
    void shutdown();

    [[nodiscard]] Statistics statistics() const;

private:
    struct ErasedResult {
        std::shared_ptr<const void> value;
        std::string error;
    };
    using ErasedWork = std::function<ErasedResult(std::stop_token)>;
    using ErasedCompletion = std::function<void(ErasedResult)>;

    struct State;
    std::unique_ptr<State> m_state;

    bool submitErased(OwnerId owner,
                      std::string key,
                      Priority priority,
                      ErasedWork work,
                      ErasedCompletion completion);
    void workerLoop(std::stop_token stop);
};

} // namespace rt
