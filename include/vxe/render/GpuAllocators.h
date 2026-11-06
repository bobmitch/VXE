#pragma once
// -----------------------------------------------------------------------------
// GPU memory sub-allocation.
//
// OffsetAllocator : best-fit range allocator with coalescing, used to place
//                   chunk meshes inside the terrain mega-buffers. O(log n).
// DeferredFrees   : holds freed ranges until the GPU has finished every frame
//                   that could still reference them.
// UploadRing      : frame-fenced linear ring over a persistently mapped
//                   staging buffer. Allocation is a pointer bump; space is
//                   reclaimed when the frame that used it completes.
// -----------------------------------------------------------------------------

#include <cassert>
#include <cstdint>
#include <deque>
#include <map>
#include <vector>

namespace vxe {

class OffsetAllocator {
public:
    static constexpr uint64_t kInvalid = ~0ull;

    explicit OffsetAllocator(uint64_t capacity) : m_capacity(capacity) { insertFree(0, capacity); }

    uint64_t allocate(uint64_t size, uint64_t align = 1) {
        if (size == 0) return kInvalid;
        for (auto it = m_bySize.lower_bound(size); it != m_bySize.end(); ++it) {
            const uint64_t blockOff = it->second, blockSize = it->first;
            const uint64_t aligned = (blockOff + align - 1) / align * align;
            const uint64_t pad = aligned - blockOff;
            if (pad + size > blockSize) continue;
            eraseFree(blockOff, blockSize, it);
            if (pad) insertFree(blockOff, pad);
            const uint64_t tail = blockSize - pad - size;
            if (tail) insertFree(aligned + size, tail);
            m_used += size;
            return aligned;
        }
        return kInvalid;
    }

    void free(uint64_t offset, uint64_t size) {
        assert(offset + size <= m_capacity);
        m_used -= size;
        // Coalesce with neighbours.
        auto next = m_byOffset.lower_bound(offset);
        if (next != m_byOffset.end() && next->first == offset + size) {
            size += next->second;
            eraseFreeByOffset(next);
            next = m_byOffset.lower_bound(offset);
        }
        if (next != m_byOffset.begin()) {
            auto prev = std::prev(next);
            if (prev->first + prev->second == offset) {
                offset = prev->first;
                size += prev->second;
                eraseFreeByOffset(prev);
            }
        }
        insertFree(offset, size);
    }

    uint64_t capacity() const { return m_capacity; }
    uint64_t usedBytes() const { return m_used; }
    uint64_t largestFreeBlock() const { return m_bySize.empty() ? 0 : m_bySize.rbegin()->first; }
    size_t freeBlockCount() const { return m_byOffset.size(); }

private:
    void insertFree(uint64_t off, uint64_t size) {
        m_byOffset.emplace(off, size);
        m_bySize.emplace(size, off);
    }
    void eraseFree(uint64_t off, uint64_t size, std::multimap<uint64_t, uint64_t>::iterator sizeIt) {
        m_bySize.erase(sizeIt);
        m_byOffset.erase(off);
        (void)size;
    }
    void eraseFreeByOffset(std::map<uint64_t, uint64_t>::iterator offIt) {
        auto range = m_bySize.equal_range(offIt->second);
        for (auto it = range.first; it != range.second; ++it)
            if (it->second == offIt->first) { m_bySize.erase(it); break; }
        m_byOffset.erase(offIt);
    }

    uint64_t m_capacity;
    uint64_t m_used = 0;
    std::map<uint64_t, uint64_t> m_byOffset;        // offset -> size
    std::multimap<uint64_t, uint64_t> m_bySize;     // size -> offset
};

class DeferredFrees {
public:
    void push(OffsetAllocator& a, uint64_t frame, uint64_t offset, uint64_t size) {
        m_pending.push_back({&a, frame, offset, size});
    }
    void collect(uint64_t completedFrame) {
        while (!m_pending.empty() && m_pending.front().frame <= completedFrame) {
            const Entry& e = m_pending.front();
            e.alloc->free(e.offset, e.size);
            m_pending.pop_front();
        }
    }
    size_t pending() const { return m_pending.size(); }

private:
    struct Entry { OffsetAllocator* alloc; uint64_t frame, offset, size; };
    std::deque<Entry> m_pending;
};

class UploadRing {
public:
    static constexpr uint64_t kInvalid = ~0ull;
    explicit UploadRing(uint64_t capacity) : m_capacity(capacity) {}

    // Returns an offset into the staging buffer valid for this frame, or kInvalid
    // if the ring is full (caller defers the upload to a later frame).
    //   not wrapped: live = [tail, head), free = [head, cap) + [0, tail)
    //   wrapped    : live = [tail, cap) + [0, head), free = [head, tail)
    uint64_t allocate(uint64_t size, uint64_t frame, uint64_t align = 16) {
        if (size == 0 || size > m_capacity) return kInvalid;
        if (m_frames.empty()) { m_head = m_tail = 0; m_wrapped = false; }
        uint64_t start = (m_head + align - 1) / align * align;
        if (!m_wrapped) {
            if (start + size > m_capacity) {
                if (size > m_tail) return kInvalid;   // no room at the front either
                start = 0;
                m_wrapped = true;                     // [old head, cap) is wasted until reclaimed
            }
        } else if (start + size > m_tail) {
            return kInvalid;
        }
        m_head = start + size;
        if (m_frames.empty() || m_frames.back().frame != frame) m_frames.push_back({frame, m_head});
        else m_frames.back().end = m_head;
        m_allocatedThisFrame += size;
        return start;
    }

    void reclaim(uint64_t completedFrame) {
        while (!m_frames.empty() && m_frames.front().frame <= completedFrame) {
            m_tail = m_frames.front().end;
            m_frames.pop_front();
            if (m_wrapped && m_tail <= m_head) m_wrapped = false;
        }
        if (m_frames.empty()) { m_head = m_tail = 0; m_wrapped = false; }
    }

    uint64_t liveBytes() const {
        if (m_frames.empty()) return 0;
        return m_wrapped ? (m_capacity - m_tail) + m_head : m_head - m_tail;
    }
    uint64_t capacity() const { return m_capacity; }

private:
    struct FrameMark { uint64_t frame, end; };
    uint64_t m_capacity;
    uint64_t m_head = 0, m_tail = 0;
    uint64_t m_allocatedThisFrame = 0;
    bool m_wrapped = false;
    std::deque<FrameMark> m_frames;
};

} // namespace vxe
