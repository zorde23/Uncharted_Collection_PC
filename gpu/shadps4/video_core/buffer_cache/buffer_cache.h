// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "gow3_copy.h"

#include <string>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>
#include <span>
#include <boost/container/small_vector.hpp>

#include "common/interval_set.h"
#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/fault_manager.h"
#include "video_core/buffer_cache/range_set.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Core {
class MemoryManager;
}

namespace Vulkan {
class GraphicsPipeline;
struct SubmitInfo;
class Runtime;
class StagingBufferPool;
} // namespace Vulkan

namespace VideoCore {

class TextureCache;
class MemoryTracker;
class PageManager;

class BufferCache {
    static constexpr u64 ADDRESS_SPACE_BITS = 40;
    // gow3: arena pages are 4 GiB unless the driver's buffer size limit is lower (AMD's
    // Windows driver: 2 GiB); an arena spans up to two pages, so they shrink to half the limit.
    static constexpr u64 MAX_ARENA_PAGE_BITS = 32;
    static constexpr u64 MIN_ARENA_PAGE_BITS = 30;
    static constexpr u64 MAX_ARENA_PAGES = u64{1} << (ADDRESS_SPACE_BITS - MIN_ARENA_PAGE_BITS);
    static constexpr u64 MIN_BLOCK_SIZE = 16_KB;

public:
    /// Read-only bindings up to this size are copied into a stream buffer (gow3: public for
    /// the draw pipeline's constant ring).
    static constexpr u64 STREAM_THRESHOLD = 16_KB;

    explicit BufferCache(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                         Vulkan::Runtime& runtime, AmdGpu::Liverpool* liverpool,
                         TextureCache& texture_cache, PageManager& tracker);
    ~BufferCache();

    /// uc3 (GOW3_READBACK_LABEL_GUARD=1): a label or fence the command processor writes to
    /// guest memory; async readbacks still in flight must not land over it with older bytes.
    static void NoteLabelWrite(VAddr address, u64 size);
    /// uc3 (GOW3_READBACK_ASYNC_ACB_SYNC=1), stage A: a dispatch from an async compute queue
    /// will write this range; read faults in its 512 KiB windows take the synchronous path.
    void NoteAcbWrite(VAddr address, u64 size);

    /// Returns a pointer to GDS device local buffer.
    [[nodiscard]] const Buffer* GetGdsBuffer() const noexcept {
        return &gds_buffer;
    }

    [[nodiscard]] const Buffer* GetNullIndexBuffer() const noexcept {
        return null_index_buffer.get();
    }

    /// Retrieves the device local DBA page table buffer.
    [[nodiscard]] Buffer* GetBdaPageTableBuffer() noexcept {
        return bda_pagetable_buffer.get();
    }

    /// Retrieves the fault buffer.
    [[nodiscard]] Buffer* GetFaultBuffer() noexcept {
        return fault_manager->GetFaultBuffer();
    }

    /// Retrieves the stream buffer.
    [[nodiscard]] StreamBuffer& GetStreamBuffer() noexcept {
        return stream_buffer;
    }

    /// Returns minimum granularity of a sparse memory bind.
    u32 GetSparsePageShift() const noexcept {
        return block_shift;
    }

    void TickFrame();

    /// Invalidates any buffer in the logical page range.
    void InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks = false,
                          bool from_fault = false);

    /// gow3: a guest write fault at `device_addr` was handled; unprotect its neighbourhood.
    void ExtendWriteFault(VAddr device_addr);

    /// Flushes any GPU modified buffer in the logical page range back to CPU memory.
    void ReadMemory(VAddr device_addr, u64 size, bool is_write = false, bool assume_locks = false,
                    bool from_fault = false);

    /// Finds a buffer for the specified region.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBuffer(VAddr device_addr, u32 size,
                                                             bool is_written,
                                                             bool is_texel_buffer = false);

    /// Attempts to obtain a buffer without modifying the cache contents.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBufferForImage(VAddr device_addr, u32 size);

    /// Return true when a region is modified from the CPU
    [[nodiscard]] bool IsRegionCpuModified(VAddr addr, size_t size);

    /// Return true when a region is modified from the GPU
    [[nodiscard]] bool IsRegionGpuModified(VAddr addr, size_t size);
    /// uc3: byte-precise: the GPU wrote some of [addr, addr + size) and it is not back in
    /// guest memory yet (gpu_modified_ranges, or an async readback still landing). GPU-side
    /// threads only.
    [[nodiscard]] bool IsGpuWritten(VAddr addr, u64 size);
    /// uc3: lets async readbacks overlapping [addr, addr + size) land in guest memory.
    void WaitReadbacksLanded(VAddr addr, u64 size) {
        WaitAsyncReadbacks(addr, addr + size);
    }
    /// uc3 (trace): "gpu_modified_ranges [start, end)", "async readback" or "image readback".
    const char* DescribeGpuWritten(VAddr addr, u64 size);
    /// uc3 (Fable R.8.4): stage B, next to a leaf's GPU copy into the constant ring: also
    /// copies the GPU bytes to host memory (unless already pending). Nobody waits;
    /// CompleteLeafDownloads lands them in guest memory once the GPU is past them.
    void QueueLeafDownload(const Buffer* buffer, u64 offset, VAddr address, u32 size);
    /// Stage A with the draw pipe drained (OnSubmit): lands finished leaf downloads whose bytes
    /// the GPU did not write again since, and clears them from gpu_modified_ranges.
    void CompleteLeafDownloads();
    /// uc3: only the gpu_modified_ranges part of IsGpuWritten (statistics).
    /// uc3 (Fable R.14 D4 diagnostics): the gpu_modified_ranges inside [addr, addr + size).
    [[nodiscard]] std::string DescribeGpuRanges(VAddr addr, u64 size);
    [[nodiscard]] bool IsGpuModifiedBytes(VAddr addr, u64 size) {
        const auto ranges_lock = RangesLock();
        return gpu_modified_ranges.Intersects(addr, size);
    }

    /// uc3 (from SuperMedo's uncharted_pc): writes the image that starts at `addr` (its current
    /// GPU contents) into the buffer of that range, before a shader copies the range as plain
    /// memory. False when no image starts there.
    bool SyncImageToBuffer(VAddr addr, u32 size);

    /// uc3 (R.8.5): GOW3_HLE_PRECISE_WAIT=1 tracks pending deferred host reads per guest page.
    static bool PreciseHostReadWait();
    /// True when a deferred host copy that reads [addr, addr+size) may still be pending.
    static bool HasPendingHostReads(VAddr addr, u64 size);

    /// Synchronizes all buffers needed for DMA.
    void SynchronizeDmaBuffers();

    /// Commits pending sparse buffer memory binds. Must be called before every scheduler submit.
    void SubmitPendingArenaBinds(Vulkan::SubmitInfo& info);

private:
    struct ArenaBinds {
        const Buffer* arena;
        boost::container::small_vector<vk::SparseMemoryBind, 32> binds;
    };

    ArenaBinds* BindsForArena(const Buffer* arena) {
        auto it = std::ranges::find(pending_binds, arena, &ArenaBinds::arena);
        if (it != pending_binds.end()) {
            return std::addressof(*it);
        }
        return &pending_binds.emplace_back(arena);
    }

    const Buffer* GetArena(u64 first_block, u64 last_block);

    void EnsureResident(const Buffer* arena, u64 first_block, u64 last_block);

    void DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size);
    /// uc3: brings every GPU-modified range of every arena back to guest memory with one
    /// GPU wait (precise readbacks; see ReadMemory).
    void DownloadAllGpuModified();
    /// uc3: downloads several windows (any arenas) with one GPU wait (GOW3_READBACK_HOT).
    struct DownloadWindow {
        const Buffer* arena;
        VAddr start;
        u64 size;
    };
    void DownloadWindows(std::span<const DownloadWindow> windows);
    /// uc3: remembers a readback window and returns the other recently read ones.
    void NoteHotWindow(VAddr start, boost::container::small_vector<VAddr, 16>& others);

    bool SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size, bool is_written,
                           bool is_texel_buffer);

    /// gow3: batched small copies on the copy threads (Gow3Copy::QueueCopy).
    static void RunGuestCopy(const Gow3Copy::Item& item);
    static void NotePendingHostRead(VAddr addr, u64 size, int delta);
    void SmallGuestCopy(const Gow3Copy::Item& item);

    const Buffer* UploadCopies(const Buffer* arena, std::span<vk::BufferCopy> copies,
                               size_t total_size_bytes);

    bool SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size);

    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    // gow3: texel buffer ranges known not to alias an image, per image registry generation.
    struct ImageMiss {
        VAddr address = 0;
        u32 size = 0;
        u64 generation = ~0ULL;
    };
    std::array<ImageMiss, 256> image_miss_cache{};
    Vulkan::Runtime& runtime;
    Vulkan::StagingBufferPool& staging_pool;
    AmdGpu::Liverpool* liverpool;
    Core::MemoryManager* memory;
    TextureCache& texture_cache;
    std::unique_ptr<MemoryTracker> memory_tracker;

    StreamBuffer stream_buffer;
    Buffer gds_buffer;
    std::unique_ptr<Buffer> null_index_buffer;
    RangeSet gpu_modified_ranges;

    std::unique_ptr<FaultManager> fault_manager;
    std::unique_ptr<Buffer> bda_pagetable_buffer;
    bool fault_process_pending{};
    /// uc3: readback windows the CPU faulted on recently (GOW3_READBACK_HOT), and the frame
    /// of the last fault on each.
    struct HotWindow {
        VAddr start{};
        u64 frame{};
    };
    std::array<HotWindow, 32> hot_windows{};
    u64 frame_index{};
    /// uc3: scheduler tick of the last GPU write bound in each 512 KiB window, direct-mapped
    /// (a slot holds the window it belongs to). Read on precise readback faults.
    struct WriteTick {
        std::atomic<VAddr> window{};
        std::atomic<u64> tick{};
    };
    static constexpr u64 WriteTickShift = 19;
    std::array<WriteTick, 4096> write_ticks{};
    std::array<std::atomic<u64>, 4096> acb_windows{}; ///< window index + 1, async compute writers
    /// Writes too large to note per window (whole arenas) raise this instead.
    std::atomic<u64> big_write_tick{};
    void NoteGpuWrite(VAddr device_addr, u64 size);
    /// uc3 (GOW3_SIDEQ_ARENA_TICK): copies into an arena that bind no written buffer.
    void NoteArenaWrite(VAddr device_addr, u64 size);
    void ClassifyFault(VAddr window_start);
    /// Tick of the last GPU write noted in [start, start + size); ~0 when a window's slot
    /// belongs to another window.
    u64 LastWriteTick(VAddr start, u64 size) const;
    struct SideCopies {
        const Buffer* arena;
        std::span<const vk::BufferCopy> copies;
    };
    /// uc3 (GOW3_READBACK_FAST): copies on the side when the GPU already finished the writes
    /// (write_tick), without submitting and draining the command buffer being recorded.
    /// False: the caller does the usual copy and Finish().
    bool SideDownload(std::span<const SideCopies> work, const Buffer* staging, u64 write_tick);
    vk::CommandPool side_pool{};
    vk::CommandBuffer side_cmdbuf{};
    vk::Fence side_fence{};
    /// uc3 R.9.16: SideDownload on the transfer queue (GOW3_READBACK_FAST_SIDEQ=1).
    vk::CommandBuffer sideq_sync_cmdbuf{};
    vk::Fence sideq_sync_fence{};

    /// uc3 (GOW3_READBACK_ASYNC=1): a read fault's download in flight. The GPU thread records
    /// the copies into a held staging buffer, submits them and goes back to its work; the
    /// faulting thread waits for `tick`, writes the data back and lifts the protection itself,
    /// unless the GPU thread bound one of the windows for writing meanwhile (`dirty`: the
    /// windows stay protected and the retried access faults again for the new data).
    struct AsyncReadback {
        u64 tick{};
        Vulkan::StagingBufferRef download{};
        boost::container::small_vector<DownloadWindow, 17> windows;
        struct Copy {
            VAddr address;
            u64 staging_offset;
            u64 size;
        };
        std::vector<Copy> copies;
        bool dirty{};                 ///< guarded by async_mutex
        std::vector<std::pair<VAddr, u64>> keep; ///< label bytes not to overwrite (async_mutex)
        std::atomic<bool> done{};
        u32 side_slot{~0u};           ///< R.9.6: copied on the side queue, waits on this slot
        u32 fast_slot{~0u};           ///< GOW3_READBACK_DONE_FAST: staging slot of its own
        /// uc3 GOW3_SIDEQ_VERIFY=1: the same copies on the graphics queue, compared on landing.
        Vulkan::StagingBufferRef verify{};
        u64 verify_tick{};
        bool has_verify{};
        bool has_wait_gfx{};   ///< GOW3_SIDEQ_WAIT_GFX=1: waits verify_tick, no copy
    };
    /// gow3 R.9.6: side-queue copies of windows whose last GPU write has finished.
    struct SideQueueSlot {
        vk::CommandBuffer cmdbuf;
        vk::Fence fence;
        bool busy{};
    };
    bool SideQueueCopy(AsyncReadback& readback,
                       std::span<const std::pair<const Buffer*, std::span<const vk::BufferCopy>>> work);
    vk::CommandPool sideq_pool{};
    std::array<SideQueueSlot, 32> sideq_slots{};
    std::mutex sideq_mutex;
    std::shared_ptr<AsyncReadback> DownloadWindowsAsync(std::span<const DownloadWindow> windows);
    void CompleteAsyncReadback(AsyncReadback& readback, VAddr device_addr, u64 size,
                               bool is_write);
    /// GPU-side threads: waits for pending async readbacks overlapping [start, end) (all of
    /// them when end == 0), and frees the staging of finished ones.
    void WaitAsyncReadbacks(VAddr start, VAddr end);
    void NoteAsyncWrite(VAddr device_addr, u64 size);
    std::mutex async_mutex;
    std::vector<std::shared_ptr<AsyncReadback>> async_pending;
    std::vector<Vulkan::StagingBufferRef> async_free;
    std::atomic<u32> async_count{};
    std::atomic<u32> async_free_count{};
    /// uc3 (GOW3_READBACK_DONE_FAST=1): a read fault on windows whose last GPU write has
    /// finished is copied on the side queue by the faulting thread itself, without queueing
    /// behind the command processor (which drains the draw pipe first: ~0.7 ms per fault in the
    /// castle vegetation). gpu_modified_ranges is then guarded by ranges_mutex (stage B adds
    /// to it), and the copy lands in a staging slot of its own (the staging pool is not
    /// thread safe).
    bool FastDoneReadback(VAddr device_addr, u64 size, bool is_write);
    [[nodiscard]] std::unique_lock<std::recursive_mutex> RangesLock();
    std::recursive_mutex ranges_mutex;
    static constexpr u64 FastSlotSize = 512_KB;
    static constexpr u32 FastSlots = 16;
    std::mutex fast_mutex;
    std::vector<u32> fast_free_slots;       ///< guarded by fast_mutex
    Vulkan::StagingBufferRef fast_staging{}; ///< allocated by the command processor thread
    std::atomic<bool> fast_ready{};
    /// uc3 (Fable R.8.4): leaf downloads (QueueLeafDownload): slots of 4..16 bytes in one
    /// host-cached allocation.
    struct LeafDownload {
        VAddr address;
        u32 size;
        u32 slot;
        u64 tick;
        bool dirty;
    };
    static constexpr u32 LeafSlotSize = 16;
    static constexpr u32 LeafSlots = 4096;
    std::mutex leaf_mutex;
    std::vector<LeafDownload> leaf_pending; ///< guarded by leaf_mutex
    std::vector<u32> leaf_free_slots;       ///< guarded by leaf_mutex
    std::atomic<u32> leaf_pending_count{};
    Vulkan::StagingBufferRef leaf_staging{};

    std::array<const Buffer*, MAX_ARENA_PAGES> address_space{};
    u64 arena_page_bits{MAX_ARENA_PAGE_BITS};
    u64 arena_page_size{u64{1} << MAX_ARENA_PAGE_BITS};
    u64 num_arena_pages{u64{1} << (ADDRESS_SPACE_BITS - MAX_ARENA_PAGE_BITS)};
    std::deque<Buffer> arenas;
    std::vector<ArenaBinds> pending_binds;
    Vulkan::Semaphore memory_semaphore;

    struct Backing : public Interval {
        vk::DeviceMemory memory;
        u64 offset;
        constexpr bool CanMergeWith(const Backing& other) const noexcept {
            return memory == other.memory && offset + (end - start) == other.offset;
        }
        constexpr Backing SubRange(u64 a, u64 b) const noexcept {
            return {{a, b}, memory, offset + (a - start)};
        }
    };
    IntervalList<Backing> resident_ranges;

    u32 arena_memory_type_index{};
    u32 block_size{};
    u32 block_shift{};
    u32 blocks_per_arena_page{};
    u32 blocks_per_arena_page_shift{};
};

} // namespace VideoCore
