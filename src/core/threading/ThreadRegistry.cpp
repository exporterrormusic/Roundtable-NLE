#include "threading/ThreadRegistry.h"

#include <algorithm>
#include <cstdio>
#include <mutex>

namespace rt {

namespace {

struct RegistryState
{
    std::timed_mutex mutex;
    std::vector<ThreadRegistry::Entry> entries;
    uint64_t nextId{1};
};

RegistryState& registryState()
{
    // Never destroyed: read by the exit-time hang report, possibly after
    // static destructors have started running.
    static auto* state = new RegistryState;
    return *state;
}

} // namespace

uint64_t ThreadRegistry::add(std::string name)
{
    auto& state = registryState();
    std::lock_guard lock(state.mutex);
    const uint64_t id = state.nextId++;
    state.entries.push_back({id, std::move(name), std::chrono::steady_clock::now()});
    return id;
}

void ThreadRegistry::remove(uint64_t id) noexcept
{
    if (id == 0) return;
    auto& state = registryState();
    std::lock_guard lock(state.mutex);
    auto it = std::find_if(state.entries.begin(), state.entries.end(),
                           [id](const Entry& e) { return e.id == id; });
    if (it != state.entries.end()) state.entries.erase(it);
}

std::vector<ThreadRegistry::Entry> ThreadRegistry::live()
{
    auto& state = registryState();
    std::lock_guard lock(state.mutex);
    return state.entries;
}

std::string ThreadRegistry::describeLive(std::chrono::milliseconds lockWait)
{
    auto& state = registryState();
    std::unique_lock lock(state.mutex, std::defer_lock);
    if (!lock.try_lock_for(lockWait)) return "(thread registry busy)";
    if (state.entries.empty()) return "none";

    const auto now = std::chrono::steady_clock::now();
    std::string out;
    for (const Entry& e : state.entries) {
        const double seconds =
            std::chrono::duration<double>(now - e.started).count();
        char age[32];
        std::snprintf(age, sizeof(age), " (%.1f s)", seconds);
        if (!out.empty()) out += ", ";
        out += e.name;
        out += age;
    }
    return out;
}

} // namespace rt
