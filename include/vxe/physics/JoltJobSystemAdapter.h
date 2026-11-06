#pragma once
// -----------------------------------------------------------------------------
// Bridges Jolt's job system onto the engine TaskSystem so physics and terrain
// jobs share one pool of worker threads (no oversubscription, one place to tune
// core affinity). Jolt jobs are queued at TaskPriority::Critical: a physics
// step is a hard frame deadline, terrain streaming is not.
//
// Jolt's barrier implementation (JobSystemWithBarrier) also lets the waiting
// physics thread execute pending jobs itself, so a saturated pool cannot
// deadlock a step.
// -----------------------------------------------------------------------------

#include <Jolt/Jolt.h>
#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Core/JobSystemWithBarrier.h>

#include <atomic>
#include <memory>

namespace vxe {

class TaskSystem;

class JoltJobSystemAdapter final : public JPH::JobSystemWithBarrier {
public:
    JoltJobSystemAdapter(TaskSystem& tasks, JPH::uint maxJobs = 2048, JPH::uint maxBarriers = 8);
    // Blocks until every pool task that references a Jolt job has finished:
    // the barrier may run a job itself, but our queued wrapper still holds a
    // reference and calls Release() -> FreeJob() on this object later.
    ~JoltJobSystemAdapter() override;

    int GetMaxConcurrency() const override;
    JobHandle CreateJob(const char* name, JPH::ColorArg color, const JobFunction& fn,
                        JPH::uint32 numDependencies = 0) override;

protected:
    void QueueJob(Job* job) override;
    void QueueJobs(Job** jobs, JPH::uint count) override;
    void FreeJob(Job* job) override;

private:
    TaskSystem& m_tasks;
    JPH::FixedSizeFreeList<Job> m_jobs;
    // Shared with in-flight wrappers so the final notify never touches a dead adapter.
    std::shared_ptr<std::atomic<int>> m_queued = std::make_shared<std::atomic<int>>(0);
};

} // namespace vxe
