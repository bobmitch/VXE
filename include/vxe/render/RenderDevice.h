#pragma once
// -----------------------------------------------------------------------------
// Thin RHI over Vulkan 1.3 / Direct3D 12. Only the surface the terrain renderer
// needs is exposed; backends live in src/render/vulkan and src/render/d3d12
// (not part of this scaffold - see README "Backend mapping").
//
// Design rules
//   * Handles are 32-bit generational ids, never API pointers.
//   * All GPU-visible terrain data lives in a few large "mega-buffers"
//     sub-allocated with OffsetAllocator; there are no per-chunk API objects.
//   * CPU->GPU traffic goes through a frame-fenced UploadRing; copies are
//     recorded into the frame's transfer queue, never waited on by the CPU.
//   * Frame latency: N frames in flight (default 2); completedFrame() tells
//     allocators when ring space / freed ranges may be reused.
// -----------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace vxe {

enum BufferUsage : uint32_t {
    kBufferVertex = 1u << 0,
    kBufferIndex = 1u << 1,
    kBufferStorage = 1u << 2,     // SRV/UAV (D3D12) / storage buffer (Vulkan)
    kBufferConstant = 1u << 3,
    kBufferIndirect = 1u << 4,
    kBufferTransferSrc = 1u << 5,
    kBufferTransferDst = 1u << 6,
};

struct BufferDesc {
    size_t size = 0;
    uint32_t usage = 0;
    bool hostVisible = false;     // upload heap / HOST_VISIBLE|COHERENT
    const char* debugName = "";
};

struct BufferHandle {
    uint32_t id = 0;
    bool valid() const { return id != 0; }
};

enum class TerrainPipelinePath : uint8_t {
    MeshShader,     // AS/MS + compute displacement for the near field (SM 6.5 / VK_EXT_mesh_shader)
    Tessellation,   // VS/HS/DS hardware tessellation (SM 5.0+ / tessellationShader)
    Classic         // VS/PS only (no micro displacement); low-end fallback
};

struct DeviceCaps {
    bool meshShaders = false;
    bool tessellation = false;
    bool bindlessTextures = false;
    uint32_t maxMeshOutputVertices = 0;
    uint32_t maxMeshOutputPrimitives = 0;
    uint32_t framesInFlight = 2;
};

inline TerrainPipelinePath selectTerrainPath(const DeviceCaps& c) {
    if (c.meshShaders && c.maxMeshOutputVertices >= 64 && c.maxMeshOutputPrimitives >= 124) return TerrainPipelinePath::MeshShader;
    if (c.tessellation) return TerrainPipelinePath::Tessellation;
    return TerrainPipelinePath::Classic;
}

class IRenderDevice {
public:
    virtual ~IRenderDevice() = default;
    virtual DeviceCaps caps() const = 0;

    virtual BufferHandle createBuffer(const BufferDesc& desc) = 0;
    virtual void destroyBuffer(BufferHandle h) = 0;      // deferred until the GPU is done with it
    virtual void* map(BufferHandle h) = 0;               // host-visible buffers only, persistently mapped
    // Recorded into the current frame's copy queue.
    virtual void copyBuffer(BufferHandle src, size_t srcOffset, BufferHandle dst, size_t dstOffset, size_t bytes) = 0;

    virtual void beginFrame() = 0;
    virtual void endFrame() = 0;                        // submit + signal fence(frameIndex)
    virtual uint64_t currentFrame() const = 0;
    virtual uint64_t completedFrame() const = 0;        // highest frame whose fence has signalled
};

// CPU-memory device used by the headless demo and tests. Frames "complete"
// `framesInFlight` frames after submission to exercise latency handling.
class NullRenderDevice final : public IRenderDevice {
public:
    explicit NullRenderDevice(DeviceCaps caps = {true, true, true, 256, 256, 2}) : m_caps(caps) {}

    DeviceCaps caps() const override { return m_caps; }
    BufferHandle createBuffer(const BufferDesc& d) override {
        m_buffers.push_back(std::make_unique<Buffer>(Buffer{d, std::vector<std::byte>(d.size)}));
        m_bytesAllocated += d.size;
        return {uint32_t(m_buffers.size())};
    }
    void destroyBuffer(BufferHandle h) override {
        if (!h.valid() || !m_buffers[h.id - 1]) return;
        m_bytesAllocated -= m_buffers[h.id - 1]->desc.size;
        m_buffers[h.id - 1].reset();
    }
    void* map(BufferHandle h) override { return m_buffers[h.id - 1]->data.data(); }
    void copyBuffer(BufferHandle src, size_t so, BufferHandle dst, size_t d, size_t n) override {
        std::memcpy(m_buffers[dst.id - 1]->data.data() + d, m_buffers[src.id - 1]->data.data() + so, n);
        m_bytesCopiedThisFrame += n;
        m_bytesCopiedTotal += n;
    }
    void beginFrame() override { m_bytesCopiedThisFrame = 0; }
    void endFrame() override { ++m_frame; }
    uint64_t currentFrame() const override { return m_frame; }
    uint64_t completedFrame() const override { return m_frame > m_caps.framesInFlight ? m_frame - m_caps.framesInFlight : 0; }

    const std::byte* data(BufferHandle h) const { return m_buffers[h.id - 1]->data.data(); }
    size_t bytesAllocated() const { return m_bytesAllocated; }
    size_t bytesCopiedTotal() const { return m_bytesCopiedTotal; }

private:
    struct Buffer { BufferDesc desc; std::vector<std::byte> data; };
    DeviceCaps m_caps;
    std::vector<std::unique_ptr<Buffer>> m_buffers;
    uint64_t m_frame = 1;
    size_t m_bytesAllocated = 0, m_bytesCopiedThisFrame = 0, m_bytesCopiedTotal = 0;
};

} // namespace vxe
