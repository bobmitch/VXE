#pragma once
// -----------------------------------------------------------------------------
// TaskSystem: a priority thread pool with dependency edges (a dynamic task
// graph). Every asynchronous pipeline in the engine is expressed as a DAG:
//
//   generate(density) --> contour(DC) --> optimise(meshopt) --+--> pack(GPU)
//                                                             +--> bake(Jolt)
//
// Threading contract
//   * submit() / wait() are callable from any thread.
//   * Task bodies must not block on other tasks except through wait(), which
//     "helps" by executing queued work instead of sleeping (no deadlock even
//     when called from a worker).
//   * Results leave the pool exclusively through lock-protected completion
//     queues owned by the consuming system; tasks never touch main-thread state.
// -----------------------------------------------------------------------------

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

namespace vxe {

enum class TaskPriority : uint8_t {
    Critical = 0,   // edits the player is waiting on (terraform remesh, collision under feet)
    High,           // LOD0 streaming around the player
    Normal,         // LOD1..N streaming
    Background,     // saves, far-field prefetch
    Count
};

class TaskSystem;

namespace detail {
struct TaskNode {
    std::function<void()> fn;
    TaskPriority priority = TaskPriority::Normal;
    std::atomic<int32_t> pending{1};    // unfinished dependencies (+1 submission guard)
    std::atomic<bool> done{false};
    std::mutex continuationMutex;
    std::vector<std::shared_ptr<TaskNode>> continuations;
    const char* name = "task";
};
} // namespace detail

class TaskHandle {
public:
    TaskHandle() = default;
    bool valid() const { return m_node != nullptr; }
    bool isDone() const { return !m_node || m_node->done.load(std::memory_order_acquire); }

private:
    friend class TaskSystem;
    explicit TaskHandle(std::shared_ptr<detail::TaskNode> n) : m_node(std::move(n)) {}
    std::shared_ptr<detail::TaskNode> m_node;
};

class TaskSystem {
public:
    // workerCount == 0 -> hardware_concurrency() - 1 (main thread is the extra core).
    explicit TaskSystem(unsigned workerCount = 0);
    ~TaskSystem();
    TaskSystem(const TaskSystem&) = delete;
    TaskSystem& operator=(const TaskSystem&) = delete;

    TaskHandle submit(std::function<void()> fn, TaskPriority priority = TaskPriority::Normal,
                      std::span<const TaskHandle> dependencies = {}, const char* name = "task");
    TaskHandle submit(std::function<void()> fn, TaskPriority priority, std::initializer_list<TaskHandle> deps,
                      const char* name = "task") {
        return submit(std::move(fn), priority, std::span<const TaskHandle>(deps.begin(), deps.size()), name);
    }

    // Blocks until the task completes, executing other queued tasks meanwhile.
    void wait(const TaskHandle& handle);
    void waitAll(std::span<const TaskHandle> handles) { for (const auto& h : handles) wait(h); }

    // Splits [0, count) into chunks of 'grain' and runs body(begin, end) in parallel.
    void parallelFor(uint32_t count, uint32_t grain, const std::function<void(uint32_t, uint32_t)>& body,
                     TaskPriority priority = TaskPriority::High);

    // Fire-and-forget convenience for continuations without return handles.
    unsigned workerCount() const { return unsigned(m_workers.size()); }
    uint64_t tasksExecuted() const { return m_executed.load(std::memory_order_relaxed); }
    size_t queuedTasks() const;

private:
    using NodePtr = std::shared_ptr<detail::TaskNode>;

    void enqueue(NodePtr node);
    bool tryRunOne();
    void execute(const NodePtr& node);
    void workerMain(std::stop_token st, unsigned index);

    mutable std::mutex m_queueMutex;
    std::condition_variable_any m_queueCv;
    std::deque<NodePtr> m_queues[size_t(TaskPriority::Count)];
    std::vector<std::jthread> m_workers;
    std::atomic<uint64_t> m_executed{0};
    std::atomic<uint32_t> m_queuedCount{0};
};

} // namespace vxe
