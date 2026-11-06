#include "vxe/core/TaskSystem.h"

#include <algorithm>
#include <cassert>

namespace vxe {

TaskSystem::TaskSystem(unsigned workerCount) {
    if (workerCount == 0) {
        const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
        workerCount = hw - 1;
    }
    m_workers.reserve(workerCount);
    for (unsigned i = 0; i < workerCount; ++i)
        m_workers.emplace_back([this, i](std::stop_token st) { workerMain(st, i); });
}

TaskSystem::~TaskSystem() {
    for (auto& w : m_workers) w.request_stop();
    m_queueCv.notify_all();
    m_workers.clear(); // joins
    // Drain anything left so dependent continuations and handles resolve.
    while (tryRunOne()) {}
}

TaskHandle TaskSystem::submit(std::function<void()> fn, TaskPriority priority, std::span<const TaskHandle> deps,
                              const char* name) {
    auto node = std::make_shared<detail::TaskNode>();
    node->fn = std::move(fn);
    node->priority = priority;
    node->name = name;
    node->pending.store(1 + int32_t(deps.size()), std::memory_order_relaxed);

    // Register as continuation of every unfinished dependency. A dependency that
    // already finished (checked under its lock) simply doesn't count.
    int32_t alreadySatisfied = 0;
    for (const TaskHandle& d : deps) {
        if (!d.m_node) { ++alreadySatisfied; continue; }
        std::lock_guard lock(d.m_node->continuationMutex);
        if (d.m_node->done.load(std::memory_order_acquire)) ++alreadySatisfied;
        else d.m_node->continuations.push_back(node);
    }
    // Remove the submission guard plus the satisfied deps; enqueue if nothing is outstanding.
    if (node->pending.fetch_sub(1 + alreadySatisfied, std::memory_order_acq_rel) == 1 + alreadySatisfied)
        enqueue(node);
    return TaskHandle(node);
}

void TaskSystem::enqueue(NodePtr node) {
    {
        std::lock_guard lock(m_queueMutex);
        m_queues[size_t(node->priority)].push_back(std::move(node));
        m_queuedCount.fetch_add(1, std::memory_order_relaxed);
    }
    m_queueCv.notify_one();
}

bool TaskSystem::tryRunOne() {
    NodePtr node;
    {
        std::lock_guard lock(m_queueMutex);
        for (auto& q : m_queues) {
            if (!q.empty()) {
                node = std::move(q.front());
                q.pop_front();
                m_queuedCount.fetch_sub(1, std::memory_order_relaxed);
                break;
            }
        }
    }
    if (!node) return false;
    execute(node);
    return true;
}

void TaskSystem::execute(const NodePtr& node) {
    if (node->fn) node->fn();
    node->fn = nullptr; // release captured resources early

    std::vector<NodePtr> ready;
    {
        std::lock_guard lock(node->continuationMutex);
        node->done.store(true, std::memory_order_release);
        ready.swap(node->continuations);
    }
    node->done.notify_all();
    m_executed.fetch_add(1, std::memory_order_relaxed);

    for (auto& c : ready)
        if (c->pending.fetch_sub(1, std::memory_order_acq_rel) == 1) enqueue(std::move(c));
}

void TaskSystem::wait(const TaskHandle& handle) {
    if (!handle.m_node) return;
    auto& node = *handle.m_node;
    while (!node.done.load(std::memory_order_acquire)) {
        if (!tryRunOne()) {
            // Nothing runnable on this thread: the task is executing elsewhere or is
            // blocked on a dependency that is executing elsewhere. Sleep until done.
            node.done.wait(false, std::memory_order_acquire);
        }
    }
}

void TaskSystem::parallelFor(uint32_t count, uint32_t grain, const std::function<void(uint32_t, uint32_t)>& body,
                             TaskPriority priority) {
    if (count == 0) return;
    grain = std::max(1u, grain);
    std::vector<TaskHandle> handles;
    handles.reserve((count + grain - 1) / grain);
    for (uint32_t b = 0; b < count; b += grain) {
        const uint32_t e = std::min(count, b + grain);
        handles.push_back(submit([&body, b, e] { body(b, e); }, priority, {}, "parallelFor"));
    }
    waitAll(handles);
}

size_t TaskSystem::queuedTasks() const { return m_queuedCount.load(std::memory_order_relaxed); }

void TaskSystem::workerMain(std::stop_token st, unsigned /*index*/) {
    while (!st.stop_requested()) {
        if (tryRunOne()) continue;
        std::unique_lock lock(m_queueMutex);
        m_queueCv.wait(lock, st, [this] { return m_queuedCount.load(std::memory_order_relaxed) > 0; });
    }
}

} // namespace vxe
