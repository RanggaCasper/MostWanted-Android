#pragma once
#include <atomic>
#include <cstdint>

namespace gpu {
// until the GPU has drawn present number n, counted from 0; any thread
void WaitGpuFrame(uint64_t n);

// perf diagnostic (XBE_PERF, or a file named perf in the app's folder): the render thread's share of the frames,
// summed until src/d3d/d3d8.cpp logs and clears them once a second
struct PerfCounters {
    std::atomic<uint64_t> render_ns{0};  // the render thread running commands
    std::atomic<uint64_t> shader_ns{0}, shaders{0};  // shader modules made, glslang included
    std::atomic<uint64_t> pipe_ns{0}, pipes{0};      // pipelines made
    std::atomic<uint64_t> tex_ns{0}, tex_bytes{0}, texes{0};  // guest textures decoded and uploaded
    std::atomic<uint64_t> stall_ns{0}, stalls{0};  // the render thread waiting on the GPU mid-frame (a full ring, a read-back)
    std::atomic<uint64_t> frame_wait_ns{0};        // and for the frame kFrames back, at present
    std::atomic<uint64_t> acquire_ns{0};           // vkAcquireNextImageKHR
    std::atomic<uint64_t> gpu_ns{0};               // the command buffers' time on the GPU
    std::atomic<uint64_t> draws{0}, submits{0};
    std::atomic<uint64_t> passes{0}, pass_px{0};  // renderings begun, and the attachment pixels they load and store
};
inline PerfCounters g_perf;

// The render thread's state changes that touched the vertex constants, counted as it applies them
// (src/d3d/render_thread.cpp), so a draw tells whether its 3 KB of constants changed without comparing them: with a
// thousand draws a frame that comparison and the copy kept for it were a tenth of the render thread on a Mali phone.
// Render thread only; g_vsc_counted is set before it starts, else draws compare
inline uint64_t g_vsc_changes = 0;
inline bool g_vsc_counted = false;
}  // namespace gpu
