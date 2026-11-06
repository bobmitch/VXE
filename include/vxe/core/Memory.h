#pragma once
// -----------------------------------------------------------------------------
// Memory primitives for streaming:
//   * BlockPool<T>      : fixed-size, slab-backed, thread-safe object pool.
//                         Density fields (233 KB each) and meshing scratch are
//                         recycled through pools so steady-state streaming does
//                         zero heap allocation for the big buffers.
//   * PoolPtr<T>        : unique_ptr that returns its block to the pool.
//   * ConcurrentQueue<T>: MPMC queue for job completions (workers -> main).
//   * SpscRing<T, N>    : lock-free single-producer/single-consumer ring,
//                         used between the main thread and the physics thread.
// -----------------------------------------------------------------------------

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <vector>

namespace vxe {

template <typename T>
class BlockPool {
public:
    explicit BlockPool(size_t blocksPerSlab = 16, size_t maxSlabs = 1024)
        : m_blocksPerSlab(blocksPerSlab), m_maxSlabs(maxSlabs) {}
    ~BlockPool() {
        assert(m_live == 0 && "BlockPool destroyed with live blocks");
        for (void* slab : m_slabs) ::operator delete(slab, std::align_val_t(alignof(T)));
    }
    BlockPool(const BlockPool&) = delete;
    BlockPool& operator=(const BlockPool&) = delete;

    struct Deleter {
        BlockPool* pool = nullptr;
        void operator()(T* p) const { if (p) pool->release(p); }
    };
    using Ptr = std::unique_ptr<T, Deleter>;

    // Returns nullptr only if the pool hit maxSlabs (back-pressure for the streamer).
    template <typename... Args>
    Ptr acquire(Args&&... args) {
        void* mem = nullptr;
        {
            std::lock_guard lock(m_mutex);
            if (m_free.empty() && !grow()) return Ptr(nullptr, Deleter{this});
            mem = m_free.back();
            m_free.pop_back();
            ++m_live;
        }
        return Ptr(new (mem) T(std::forward<Args>(args)...), Deleter{this});
    }

    size_t liveBlocks() const { std::lock_guard l(m_mutex); return m_live; }
    size_t capacityBlocks() const { std::lock_guard l(m_mutex); return m_slabs.size() * m_blocksPerSlab; }
    size_t bytesReserved() const { return capacityBlocks() * sizeof(T); }

private:
    void release(T* p) {
        p->~T();
        std::lock_guard lock(m_mutex);
        m_free.push_back(p);
        --m_live;
    }
    bool grow() {
        if (m_slabs.size() >= m_maxSlabs) return false;
        auto* slab = static_cast<std::byte*>(::operator new(sizeof(T) * m_blocksPerSlab, std::align_val_t(alignof(T))));
        m_slabs.push_back(slab);
        for (size_t i = 0; i < m_blocksPerSlab; ++i) m_free.push_back(slab + i * sizeof(T));
        return true;
    }

    const size_t m_blocksPerSlab;
    const size_t m_maxSlabs;
    mutable std::mutex m_mutex;
    std::vector<void*> m_slabs;
    std::vector<void*> m_free;
    size_t m_live = 0;
};

template <typename T>
using PoolPtr = typename BlockPool<T>::Ptr;

// Multi-producer multi-consumer queue. Completion traffic is a few hundred
// items per frame, so a mutex + swap-drain beats a lock-free design on clarity
// with no measurable cost; the consumer drains in O(1) lock time.
template <typename T>
class ConcurrentQueue {
public:
    void push(T v) {
        std::lock_guard lock(m_mutex);
        m_items.push_back(std::move(v));
    }
    // Swaps out everything queued so far.
    std::vector<T> drain() {
        std::vector<T> out;
        std::lock_guard lock(m_mutex);
        out.swap(m_items);
        return out;
    }
    size_t sizeApprox() const { std::lock_guard l(m_mutex); return m_items.size(); }

private:
    mutable std::mutex m_mutex;
    std::vector<T> m_items;
};

// Lock-free bounded SPSC ring. N must be a power of two.
template <typename T, size_t N>
class SpscRing {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");

public:
    bool tryPush(T v) {
        const size_t head = m_head.load(std::memory_order_relaxed);
        if (head - m_tail.load(std::memory_order_acquire) == N) return false;
        m_slots[head & (N - 1)] = std::move(v);
        m_head.store(head + 1, std::memory_order_release);
        return true;
    }
    std::optional<T> tryPop() {
        const size_t tail = m_tail.load(std::memory_order_relaxed);
        if (tail == m_head.load(std::memory_order_acquire)) return std::nullopt;
        T v = std::move(m_slots[tail & (N - 1)]);
        m_tail.store(tail + 1, std::memory_order_release);
        return v;
    }
    size_t sizeApprox() const { return m_head.load(std::memory_order_acquire) - m_tail.load(std::memory_order_acquire); }

private:
    std::array<T, N> m_slots{};
    alignas(64) std::atomic<size_t> m_head{0};
    alignas(64) std::atomic<size_t> m_tail{0};
};

} // namespace vxe
