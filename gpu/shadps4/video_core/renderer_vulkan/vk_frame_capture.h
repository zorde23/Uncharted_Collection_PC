// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: frame analyzer. Creating the file named by GOW3_CAPTURE_TRIGGER records the next full
// frame: every render pass (targets, depth, draw count, shaders, sampled textures) and compute
// dispatch, in order, plus the buffer that was presented. Input for placing a temporal
// upscaler (scene color, depth, motion vectors, UI) in the game's frame.

#pragma once

#include <array>

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#include "common/types.h"

namespace VideoCore {
struct ImageInfo;
}

namespace Vulkan {

class FrameCapture {
public:
    /// VideoOut thread, per flip: frame boundary, presented buffer, trigger file check.
    static void OnFlip(VAddr presented_address);

    /// VideoOut: a display buffer the game registered. A pass drawing into one starts a frame
    /// in the GPU command stream (the flip itself runs a frame behind).
    static void AddDisplayBuffer(VAddr address);
    static bool IsDisplayBuffer(VAddr address);

    /// GPU thread: whether passes are being looked at (armed or recording; cheap).
    static bool Active() {
        return state.load(std::memory_order_relaxed) != Idle;
    }
    /// GPU thread, at every draw/dispatch: starts or ends a recording at frame boundaries.
    static void Poll();

    static void BeginPass(const VideoCore::ImageInfo* const* colors, u32 num_colors,
                          const VideoCore::ImageInfo* depth);
    static void Draw(u64 vs_hash, u64 ps_hash, u32 num_indices, u32 num_instances);
    static void Dispatch(u64 cs_hash, u32 x, u32 y, u32 z);
    /// `detail`: mips and layers of the cache image, the T# and the view (uc3 sand sparkle).
    static void Sampled(const VideoCore::ImageInfo& info, bool storage,
                        const std::string& detail = {});
    /// uc3: the S# of a sampler bound for the next draw/dispatch, as text.
    static void SamplerState(std::string text);
    /// Contents of a bound buffer (first 1 KiB), kept for small passes and a pass's first draw.
    /// uc3: callers copy at most this much of a guest buffer (Buffer prints 1 KiB).
    using CapturedBytes = std::array<u8, 4096>;
    static void Buffer(u64 stage_hash, u32 slot, VAddr address, const void* data, u64 size);
    /// uc1 (white crates): a free text line printed with the draw's buffers.
    static void BufferText(std::string text);
    static void Note(const char* text);

    /// uc3 (GOW3_DUMP_CAPTURE): the number of the captured frame the GPU stream is in (its
    /// passes or the display pass that ends it), 0 outside captures; and whether the current
    /// pass is that display pass.
    static u32 DumpFrame();
    static bool InDisplayPass();

    /// In-game menu: records the next full frame (as the trigger file does).
    static void Request();
    /// What the last request did, for the menu: empty, "Capturing...", the file or an error.
    static std::string LastResult();

private:
    /// Tail: after the last frame is written, until its display pass ends (GOW3_DUMP_CAPTURE).
    enum : u32 { Idle, Armed, Recording, Tail };
    static inline std::atomic<u32> state{Idle};
    static inline std::atomic<u64> flips{0};
    static inline std::atomic<VAddr> last_presented{0};
    static inline std::atomic<bool> requested{false};
};

} // namespace Vulkan
