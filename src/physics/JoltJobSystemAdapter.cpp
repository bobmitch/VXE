#include "vxe/physics/JoltJobSystemAdapter.h"

#include "vxe/core/TaskSystem.h"

#include <chrono>
#include <thread>

namespace vxe {

JoltJobSystemAdapter::JoltJobSystemAdapter(TaskSystem& tasks, JPH::uint maxJobs, JPH::uint maxBarriers)
    : m_tasks(tasks) {
    JobSystemWithBarrier::Init(maxBarriers);
    m_jobs.Init(maxJobs, maxJobs);
}

JoltJobSystemAdapter::~JoltJobSystemAdapter() {
    for (int n = m_queued->load(std::memory_order_acquire); n != 0; n = m_queued->load(std::memory_order_acquire))
        m_queued->wait(n, std::memory_order_acquire);
}

int JoltJobSystemAdapter::GetMaxConcurrency() const { return int(m_tasks.workerCount()) + 1; }

JPH::JobSystem::JobHandle JoltJobSystemAdapter::CreateJob(const char* name, JPH::ColorArg color,
                                                          const JobFunction& fn, JPH::uint32 numDependencies) {
    JPH::uint32 index;
    for (;;) {
        index = m_jobs.ConstructObject(name, color, this, fn, numDependencies);
        if (index != decltype(m_jobs)::cInvalidObjectIndex) break;
        // Pool exhausted: let workers drain. Size maxJobs so this never happens in practice.
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    Job* job = &m_jobs.Get(index);
    JobHandle handle(job);           // holds a reference
    if (numDependencies == 0) QueueJob(job);
    return handle;
}

void JoltJobSystemAdapter::QueueJob(Job* job) {
    job->AddRef();                   // released after execution
    m_queued->fetch_add(1, std::memory_order_relaxed);
    m_tasks.submit([job, queued = m_queued] {
        job->Execute();              // no-op if the barrier already ran it
        job->Release();              // may FreeJob() on the adapter: it is still alive (dtor waits)
        if (queued->fetch_sub(1, std::memory_order_acq_rel) == 1) queued->notify_all();
    }, TaskPriority::Critical, {}, "jolt.job");
}

void JoltJobSystemAdapter::QueueJobs(Job** jobs, JPH::uint count) {
    for (JPH::uint i = 0; i < count; ++i) QueueJob(jobs[i]);
}

void JoltJobSystemAdapter::FreeJob(Job* job) { m_jobs.DestructObject(job); }

} // namespace vxe
