// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <limits>
#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <unordered_map>
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <thread>
#include <cstdio>
#include <string>
#include <vector>
#include <magic_enum/magic_enum.hpp>
#include "gow3_copy.h"
#include "gow3_toggles.h"
#include "gow3_write_log.h"
#include "common/alignment.h"
#include "common/thread.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/memory_tracker.h"
#include "video_core/buffer_cache/region_definitions.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

#include <vk_mem_alloc.h>

namespace VideoCore {

static constexpr size_t GDS_BUFFER_SIZE = 64_KB;
static constexpr size_t NULL_INDEX_BUFFER_SIZE = 16;
static constexpr size_t STREAM_BUFFER_SIZE = 128_MB;
constexpr std::optional<u32> CmaskColorExpandedValue(u32 num_samples) {
    switch (num_samples) {
    case 1:
        return 0xFFFFFFFF;
    case 2:
        return 0xDDDDDDDD;
    case 4:
        return 0xEEEEEEEE;
    case 8:
        return 0xFFFFFFFF;
    default:
        return 0;
    }
}
static_assert(CmaskColorExpandedValue(1) == 0xFFFFFFFF);
static_assert(CmaskColorExpandedValue(2) == 0xDDDDDDDD);
static_assert(CmaskColorExpandedValue(4) == 0xEEEEEEEE);
static_assert(CmaskColorExpandedValue(8) == 0xFFFFFFFF);

static constexpr auto ARENA_USAGE =
    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
    vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
    vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eVertexBuffer |
    vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress;

std::optional<u32> FindMemoryType(const vk::PhysicalDeviceMemoryProperties& properties,
                                  vk::MemoryPropertyFlags wanted, u32 memory_type_bits) {
    for (u32 i = 0; i < properties.memoryTypeCount; ++i) {
        if (((memory_type_bits >> i) & 1) == 0) {
            continue;
        }
        const auto flags = properties.memoryTypes[i].propertyFlags;
        if ((flags & wanted) == wanted) {
            return i;
        }
    }
    return std::nullopt;
}

namespace {
BufferCache* label_guard_instance = nullptr;

bool ReadbackLabelGuard() {
    static const bool enabled = EmulatorSettingsImpl::Flag("GOW3_READBACK_LABEL_GUARD", false);
    return enabled;
}
std::atomic<u64> label_guard_hits{0};
} // namespace

static bool ReadbackAcbSync() {
    static const bool enabled = EmulatorSettingsImpl::Flag("GOW3_READBACK_ASYNC_ACB_SYNC", false);
    return enabled;
}

void BufferCache::NoteAcbWrite(VAddr address, u64 size) {
    if (!ReadbackAcbSync() || size == 0) {
        return;
    }
    const VAddr first = address >> WriteTickShift;
    const VAddr last = std::min<VAddr>((address + size - 1) >> WriteTickShift, first + 63);
    for (VAddr window = first; window <= last; ++window) {
        if (acb_windows[window % acb_windows.size()].exchange(window + 1, std::memory_order_relaxed) !=
            window + 1) {
            static std::atomic<u32> marked{0};
            const u32 n = marked.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 4 || (n & 15) == 0) {
                std::printf("uc3: async compute windows marked: %u\n", n);
            }
        }
    }
}

void BufferCache::NoteLabelWrite(VAddr address, u64 size) {
    auto* self = label_guard_instance;
    if (!self || !ReadbackLabelGuard() || self->async_count.load(std::memory_order_acquire) == 0) {
        return;
    }
    std::scoped_lock lk{self->async_mutex};
    for (const auto& entry : self->async_pending) {
        for (const auto& copy : entry->copies) {
            if (copy.address < address + size && address < copy.address + copy.size) {
                entry->keep.emplace_back(address, size);
                break;
            }
        }
    }
}

BufferCache::BufferCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                         Vulkan::Runtime& runtime_, AmdGpu::Liverpool* liverpool_,
                         TextureCache& texture_cache_, PageManager& tracker)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_},
      staging_pool{runtime_.GetStagingPool()}, liverpool{liverpool_},
      memory{Core::Memory::Instance()}, texture_cache{texture_cache_},
      memory_tracker{std::make_unique<MemoryTracker>(tracker)},
      stream_buffer{instance, scheduler, MemoryType::Stream, STREAM_BUFFER_SIZE},
      gds_buffer{instance, 0, GDS_BUFFER_SIZE, MemoryType::Stream, "GDS Buffer"},
      memory_semaphore{instance, "memory"} {
    label_guard_instance = this;
    while (arena_page_bits > MIN_ARENA_PAGE_BITS &&
           (u64{2} << arena_page_bits) > instance.GetMaxBufferSize()) {
        --arena_page_bits;
    }
    arena_page_size = u64{1} << arena_page_bits;
    num_arena_pages = u64{1} << (ADDRESS_SPACE_BITS - arena_page_bits);
    if (arena_page_bits != MAX_ARENA_PAGE_BITS) {
        LOG_INFO(Render_Vulkan, "Buffer arenas of {} MiB (driver buffer limit {} MiB)",
                 arena_page_size >> 20, instance.GetMaxBufferSize() >> 20);
    }
    const vk::BufferCreateInfo probe_ci = {
        .flags =
            vk::BufferCreateFlagBits::eSparseBinding | vk::BufferCreateFlagBits::eSparseResidency,
        .size = arena_page_size,
        .usage = ARENA_USAGE,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    const vk::DeviceBufferMemoryRequirements req_info = {
        .pCreateInfo = &probe_ci,
    };
    const auto device = instance.GetDevice();
    const auto reqs = device.getBufferMemoryRequirements(req_info).memoryRequirements;
    block_size = Common::AlignUp(std::max<u64>(reqs.alignment, MIN_BLOCK_SIZE), reqs.alignment);
    ASSERT_MSG(std::popcount(block_size) == 1, "Sparse block size {} is not a power of 2",
               block_size);
    block_shift = std::bit_width(block_size) - 1;
    blocks_per_arena_page = arena_page_size / block_size;
    blocks_per_arena_page_shift = arena_page_bits - block_shift;
    arena_memory_type_index =
        FindMemoryType(instance.GetMemoryProperties(), vk::MemoryPropertyFlagBits::eDeviceLocal,
                       reqs.memoryTypeBits)
            .value();

    const u64 bda_pagetable_size =
        (blocks_per_arena_page * num_arena_pages) * sizeof(vk::DeviceAddress);
    fault_manager = std::make_unique<FaultManager>(instance, scheduler, *this, block_shift,
                                                   blocks_per_arena_page * num_arena_pages);
    bda_pagetable_buffer = std::make_unique<Buffer>(
        instance, 0, bda_pagetable_size, MemoryType::DeviceLocal, "BDA Page Table Buffer");
    runtime.FillBuffer(bda_pagetable_buffer.get(), 0u, bda_pagetable_size, 0u);
    if (!instance.IsMaintenance6Supported()) {
        null_index_buffer = std::make_unique<Buffer>(instance, 0, NULL_INDEX_BUFFER_SIZE,
                                                     MemoryType::DeviceLocal, "Null Index Buffer");
        runtime.FillBuffer(null_index_buffer.get(), 0u, NULL_INDEX_BUFFER_SIZE, 0u);
    }
}

BufferCache::~BufferCache() {
    const vk::Device device = instance.GetDevice();
    if (sideq_pool) {
        if (instance.GetSideQueue()) {
            (void)instance.GetSideQueue().waitIdle();
        }
        for (auto& slot : sideq_slots) {
            if (slot.fence) {
                device.destroyFence(slot.fence);
            }
        }
        device.destroyCommandPool(sideq_pool);
    }
    if (side_fence) {
        device.destroyFence(side_fence);
    }
    if (side_pool) {
        device.destroyCommandPool(side_pool);
    }
}

void BufferCache::TickFrame() {
    ++frame_index;
    WaitAsyncReadbacks(1, 1); // an empty range: only frees finished readbacks' staging
    if (std::exchange(fault_process_pending, false)) {
        fault_manager->ProcessFaultBuffer();
    }
}

void BufferCache::InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks,
                                   bool from_fault) {
    memory_tracker->InvalidateRegion(device_addr, size,
                                     [this, device_addr, size, assume_locks, from_fault] {
        ReadMemory(device_addr, size, true, assume_locks, from_fault);
    });
}

// gow3: the game fills its per-frame buffers (constants, skinning output) sequentially and
// every 4 KiB page cost a protection fault (~110k/s in the test scene, a fifth of each
// render worker's time in the kernel). A fault unprotects the aligned window around it instead;
// pages marked CPU-modified without being written only cost an upload when bound.
// Item: context = BufferCache, source = guest address, destination = host pointer, size,
// extra = the Buffer whose mapping holds the destination (flushed after the copy).
void BufferCache::RunGuestCopy(const Gow3Copy::Item& item) {
    auto* cache = static_cast<BufferCache*>(item.context);
    auto* dst = reinterpret_cast<u8*>(item.destination);
    cache->memory->CopySparseMemory(item.source, dst, item.size);
    auto* buffer = reinterpret_cast<Buffer*>(item.extra);
    buffer->Flush(dst - buffer->mapped_data.data(), item.size);
    NotePendingHostRead(item.source, item.size, -1);
}

// uc3 (Fable R.8.5): deferred host copies read guest memory later, on the recording and copy
// threads. A counter per 64 KiB guest page (hashed; a collision only costs an extra wait) says
// whether a read of that page is still pending, so the scalar copy HLE waits only when it is
// about to overwrite bytes an earlier upload has not read yet.
static constexpr u32 PendingReadBuckets = 1u << 16;
static std::array<std::atomic<u32>, PendingReadBuckets> pending_host_reads{};

static u32 PendingReadBucket(VAddr page) {
    return static_cast<u32>((page * 0x9E3779B97F4A7C15ULL) >> 48) & (PendingReadBuckets - 1);
}

bool BufferCache::PreciseHostReadWait() {
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_HLE_PRECISE_WAIT");
        return env && env[0] == '1';
    }();
    return enabled;
}

void BufferCache::NotePendingHostRead(VAddr addr, u64 size, int delta) {
    if (!PreciseHostReadWait() || size == 0) {
        return;
    }
    for (VAddr page = addr >> 16; page <= (addr + size - 1) >> 16; ++page) {
        auto& bucket = pending_host_reads[PendingReadBucket(page)];
        if (delta > 0) {
            bucket.fetch_add(1, std::memory_order_release);
        } else {
            bucket.fetch_sub(1, std::memory_order_release);
        }
    }
}

bool BufferCache::HasPendingHostReads(VAddr addr, u64 size) {
    if (size == 0) {
        return false;
    }
    for (VAddr page = addr >> 16; page <= (addr + size - 1) >> 16; ++page) {
        if (pending_host_reads[PendingReadBucket(page)].load(std::memory_order_acquire) != 0) {
            return true;
        }
    }
    return false;
}

// Small guest copies run on the recording thread (it spins for work: no wakeup, and it is
// idle most of the time); PoolSmallCopies (toggle 524288) batches them for the copy threads.
void BufferCache::SmallGuestCopy(const Gow3Copy::Item& item) {
    NotePendingHostRead(item.source, item.size, +1);
    if (scheduler.IsRecordingDeferred() && !Gow3Toggle::Disabled(Gow3Toggle::PoolSmallCopies)) {
        scheduler.RecordHostCopy([item] { item.run(item); });
        return;
    }
    Gow3Copy::QueueCopy(item);
}

void BufferCache::ExtendWriteFault(VAddr device_addr) {
    static const u64 window = [] {
        const char* env = std::getenv("GOW3_FAULT_WINDOW");
        const u64 kib = env ? std::strtoull(env, nullptr, 10) : 256;
        return std::bit_ceil(std::clamp<u64>(kib, 4, 1024)) * 1024;
    }();
    if (window <= BYTES_PER_PAGE || Gow3Toggle::Disabled(Gow3Toggle::FaultWindow)) {
        return;
    }
    memory_tracker->ExtendWriteFault(Common::AlignDown(device_addr, window), window);
}

// uc3: precise readbacks cost a fault, a 512 KiB window download and a full GPU wait per CPU
// access to a page the GPU wrote. GOW3_READBACK_ALL=1 makes one readback bring back every
// GPU-modified range of every arena with one wait, so the accesses that follow find nothing
// pending. OFF by default: it also brings back ranges the CPU never asked for, and a range
// marked GPU-modified at bind time whose memory the game has since reused (textures loaded
// from disk after a level reload) gets the arena's stale bytes written over the new data:
// corrupted textures and lightmaps after "menu -> new game" in Uncharted 3. The window
// download only touches what the CPU is reading, where the GPU's data is what it wants.
static bool ReadbackAll() {
    static const bool all = EmulatorSettings.GetReadbacksMode() == GpuReadbacksMode::Precise &&
                            EmulatorSettingsImpl::Flag("GOW3_READBACK_ALL", false);
    return all;
}

// uc3: the CPU reads the same GPU-written windows frame after frame (7 faults, 20 ms in one
// 50 ms frame at The Chateau), and each fault drains the GPU queue on its own.
// GOW3_READBACK_HOT=1: a fault also brings back the other windows faulted on in the last
// few frames, if the GPU wrote them again, under the same wait; the faults that would follow
// in this frame find them clean. Unlike GOW3_READBACK_ALL it only touches windows the CPU has
// been reading, where the GPU's data is what it wants. OFF by default until measured.
static bool ReadbackHot() {
    static const bool hot = EmulatorSettings.GetReadbacksMode() == GpuReadbacksMode::Precise &&
                            EmulatorSettingsImpl::Flag("GOW3_READBACK_HOT", false);
    return hot;
}

void BufferCache::NoteHotWindow(VAddr start, boost::container::small_vector<VAddr, 16>& others) {
    constexpr u64 HotFrames = 4;
    HotWindow* slot = nullptr;
    for (auto& window : hot_windows) {
        if (window.start == start) {
            slot = &window;
        } else if (window.start && frame_index - window.frame <= HotFrames &&
                   others.size() < others.capacity()) {
            others.push_back(window.start);
        }
    }
    if (!slot) {
        // Reuse the oldest slot.
        slot = &*std::ranges::min_element(hot_windows, {}, &HotWindow::frame);
    }
    *slot = HotWindow{start, frame_index};
}

// uc3 (GOW3_FAULT_CATEGORY=1, diagnostic): time the faulting threads wait, per kind of fault
// (the last GPU write to the window still open, in flight or done), and whether the faulted
// pages themselves were written earlier than the 512 KiB window (a false dependency). Pages
// are tracked only in windows that faulted before.
namespace {
bool FaultCategoryDiag() {
    static const bool enabled = EmulatorSettingsImpl::Flag("GOW3_FAULT_CATEGORY", false);
    return enabled;
}
constexpr u64 DiagPageShift = 12;
std::array<std::atomic<u64>, 64> diag_fault_windows{}; // window index + 1
std::atomic<u32> diag_fault_window_next{0};
std::array<std::atomic<u64>, 65536> diag_page_key{};  // page index + 1
std::array<std::atomic<u64>, 65536> diag_page_tick{};
// Fable R.14 D0 (5): who last wrote a page / window (Gow3Stats::diag_writer at the binding).
std::array<std::atomic<u64>, 65536> diag_page_writer{};
std::array<std::atomic<u64>, 4096> diag_window_key{}; // window + 1
std::array<std::atomic<u64>, 4096> diag_window_writer{};
bool DiagWindowFaulted(VAddr window) {
    for (const auto& slot : diag_fault_windows) {
        if (slot.load(std::memory_order_relaxed) == window + 1) {
            return true;
        }
    }
    return false;
}
u64 DiagPageTick(VAddr page) {
    const size_t slot = page % diag_page_key.size();
    return diag_page_key[slot].load(std::memory_order_relaxed) == page + 1
               ? diag_page_tick[slot].load(std::memory_order_relaxed)
               : 0;
}
struct DiagTotals {
    std::atomic<u64> count[4]{};
    std::atomic<u64> ns[4]{};
    std::atomic<u64> false_count{};
    std::atomic<u64> false_ns{};
    std::atomic<u64> page_unknown{};
    std::atomic<u64> last_print_ns{};
    // Per category: queued for the command processor, on the command processor, waiting for
    // the copy to land (the rest of ns[] is marking/copying out).
    std::atomic<u64> queue_ns[4]{};
    std::atomic<u64> cp_ns[4]{};
    std::atomic<u64> land_ns[4]{};
} diag_totals;
// Fable R.14 (c): which game threads fault, by name (first 32 threads that fault).
struct DiagThread {
    std::atomic<bool> used{false};
    char name[48]{};
    std::atomic<u64> count{0};
    std::atomic<u64> ns{0};
};
std::array<DiagThread, 32> diag_threads{};
std::atomic<u32> diag_thread_next{0};
DiagThread* DiagThisThread() {
    thread_local DiagThread* self = [] () -> DiagThread* {
        const u32 index = diag_thread_next.fetch_add(1, std::memory_order_relaxed);
        if (index >= diag_threads.size()) {
            return nullptr;
        }
        auto& slot = diag_threads[index];
        const std::string name = Common::GetCurrentThreadName();
        std::snprintf(slot.name, sizeof(slot.name), "%s#%u", name.empty() ? "?" : name.c_str(),
                      index);
        slot.used.store(true, std::memory_order_release);
        return &slot;
    }();
    return self;
}
// Fable R.14 D0 (5): faults of the GfxEop handler (0) and NdJobWorkerThread 0 (1): last writer
// of the faulted page (window when the page is untracked), and a few addresses per window.
int DiagWatchedThread() {
    thread_local const int kind = [] {
        const std::string name = Common::GetCurrentThreadName();
        return name.starts_with("GPU Interrupts") ? 0 : name == "NdJobWorkerThread 0" ? 1 : -1;
    }();
    return kind;
}
struct DiagWriterRow {
    u64 count = 0;
    u64 ns = 0;
};
std::mutex diag_writer_mutex;
std::map<u64, DiagWriterRow> diag_writers[2];
struct DiagSample {
    VAddr addr;
    u64 size;
    int category;
    u64 writer;
    bool from_page;
    std::string info;
};
std::vector<DiagSample> diag_samples[2];
u64 DiagWriterOf(VAddr addr, bool& from_page) {
    const VAddr page = addr >> DiagPageShift;
    const size_t index = page % diag_page_key.size();
    if (diag_page_key[index].load(std::memory_order_relaxed) == page + 1) {
        from_page = true;
        return diag_page_writer[index].load(std::memory_order_relaxed);
    }
    from_page = false;
    const VAddr window = addr >> 19; // WriteTickShift (512 KiB windows)
    const size_t w = window % diag_window_key.size();
    return diag_window_key[w].load(std::memory_order_relaxed) == window + 1
               ? diag_window_writer[w].load(std::memory_order_relaxed)
               : 0;
}
} // namespace

void BufferCache::NoteGpuWrite(VAddr device_addr, u64 size) {
    const u64 tick = scheduler.CurrentTick();
    const VAddr first = device_addr >> WriteTickShift;
    const VAddr last = (device_addr + size - 1) >> WriteTickShift;
    // Huge bindings (whole arenas) would touch every slot; the readback windows are small.
    if (last - first >= 64) {
        big_write_tick.store(tick, std::memory_order_relaxed);
        return;
    }
    for (VAddr window = first; window <= last; ++window) {
        auto& slot = write_ticks[window % write_ticks.size()];
        slot.window.store(window, std::memory_order_relaxed);
        slot.tick.store(tick, std::memory_order_relaxed);
        if (FaultCategoryDiag()) {
            const u64 writer = Gow3Stats::diag_writer.load(std::memory_order_relaxed);
            diag_window_key[window % diag_window_key.size()].store(window + 1,
                                                                    std::memory_order_relaxed);
            diag_window_writer[window % diag_window_key.size()].store(writer,
                                                                       std::memory_order_relaxed);
        }
        if (FaultCategoryDiag() && DiagWindowFaulted(window)) {
            const VAddr begin = std::max<VAddr>(device_addr, window << WriteTickShift);
            const VAddr end = std::min<VAddr>(device_addr + size, (window + 1) << WriteTickShift);
            for (VAddr page = begin >> DiagPageShift; page <= (end - 1) >> DiagPageShift; ++page) {
                const size_t index = page % diag_page_key.size();
                diag_page_key[index].store(page + 1, std::memory_order_relaxed);
                diag_page_tick[index].store(tick, std::memory_order_relaxed);
                diag_page_writer[index].store(Gow3Stats::diag_writer.load(std::memory_order_relaxed),
                                              std::memory_order_relaxed);
            }
        }
    }
}

// uc3 (Drake's scarf, Opus causa-sideq.md): only written bindings note a write tick, but
// uploads, metadata fills and image tiling also write arenas. With GOW3_SIDEQ_ARENA_TICK=1
// they note one too, so the side queue never copies a window before those land.
static bool SideqArenaTick() {
    static const bool enabled = EmulatorSettingsImpl::Flag("GOW3_SIDEQ_ARENA_TICK", false);
    return enabled;
}

void BufferCache::NoteArenaWrite(VAddr device_addr, u64 size) {
    if (!SideqArenaTick() || size == 0) {
        return;
    }
    const u64 tick = scheduler.CurrentTick();
    const VAddr first = device_addr >> WriteTickShift;
    const VAddr last = (device_addr + size - 1) >> WriteTickShift;
    if (last - first >= 64) {
        big_write_tick.store(tick, std::memory_order_relaxed);
        return;
    }
    for (VAddr window = first; window <= last; ++window) {
        auto& slot = write_ticks[window % write_ticks.size()];
        slot.window.store(window, std::memory_order_relaxed);
        slot.tick.store(tick, std::memory_order_relaxed);
    }
}

u64 BufferCache::LastWriteTick(VAddr start, u64 size) const {
    u64 tick = big_write_tick.load(std::memory_order_relaxed);
    const VAddr last = (start + size - 1) >> WriteTickShift;
    for (VAddr window = start >> WriteTickShift; window <= last; ++window) {
        const auto& slot = write_ticks[window % write_ticks.size()];
        if (slot.window.load(std::memory_order_relaxed) != window) {
            return ~0ULL;
        }
        tick = std::max(tick, slot.tick.load(std::memory_order_relaxed));
    }
    return tick;
}

// uc3: most precise readback faults (about 85% in the streets of Cartagena) read windows whose
// last GPU write has already finished; the fault still submitted the command buffer being
// recorded and waited for all of it. GOW3_READBACK_FAST=1 copies those on the side: a command
// buffer of its own on the graphics queue runs right after the finished work. The noted tick
// is the one current when the buffer was bound; the draw can land in the next command buffer
// if a submit comes between, so the tick after it must be finished too.
static bool ReadbackFast() {
    static const bool fast = EmulatorSettings.GetReadbacksMode() == GpuReadbacksMode::Precise &&
                             EmulatorSettingsImpl::Flag("GOW3_READBACK_FAST", false);
    return fast;
}

bool BufferCache::SideDownload(std::span<const SideCopies> work, const Buffer* staging,
                               u64 write_tick) {
    if (!ReadbackFast() || write_tick == ~0ULL) {
        return false;
    }
    auto* semaphore = scheduler.GetWorkSemaphore();
    if (semaphore->KnownGpuTick() < write_tick + 1) {
        semaphore->Refresh();
        if (semaphore->KnownGpuTick() < write_tick + 1) {
            // uc3 R.9.15 (GOW3_READBACK_FAST_WAIT=1): when the write's command buffers are
            // already committed, wait only for them instead of submitting the open command
            // buffer and waiting for all queued work (scheduler.Finish()).
            static const bool wait_write = EmulatorSettingsImpl::Flag("GOW3_READBACK_FAST_WAIT", false);
            if (!wait_write || write_tick + 1 >= scheduler.CurrentTick()) {
                return false;
            }
            scheduler.Wait(write_tick + 1);
            Gow3Stats::uc3_rb_side_waits.fetch_add(1, std::memory_order_relaxed);
        }
    }
    const vk::Device device = instance.GetDevice();
    // uc3 R.9.16 (GOW3_READBACK_FAST_SIDEQ=1): the copy goes to the transfer queue, so it does
    // not wait behind the work queued on the graphics queue after the write (the side command
    // buffer there cost ~0.3 ms per readback in the castle). The finished write tick carries
    // the memory dependency, as in SideQueueCopy.
    static const bool use_sideq = EmulatorSettingsImpl::Flag("GOW3_READBACK_FAST_SIDEQ", false);
    if (use_sideq && instance.GetSideQueue()) {
        const auto start = std::chrono::steady_clock::now();
        {
            std::scoped_lock lk{sideq_mutex};
            if (!sideq_pool) {
                sideq_pool = Vulkan::Check<"create side queue command pool">(device.createCommandPool({
                    .flags = vk::CommandPoolCreateFlagBits::eTransient |
                             vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
                    .queueFamilyIndex = instance.GetSideQueueFamilyIndex(),
                }));
            }
            if (!sideq_sync_cmdbuf) {
                const vk::CommandBufferAllocateInfo alloc_info = {
                    .commandPool = sideq_pool,
                    .level = vk::CommandBufferLevel::ePrimary,
                    .commandBufferCount = 1U,
                };
                Vulkan::Check<"allocate side queue command buffer">(
                    device.allocateCommandBuffers(&alloc_info, &sideq_sync_cmdbuf));
                sideq_sync_fence = Vulkan::Check<"create side queue fence">(device.createFence({}));
            }
            Vulkan::Check(sideq_sync_cmdbuf.reset());
            Vulkan::Check(sideq_sync_cmdbuf.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}));
            for (const auto& entry : work) {
                sideq_sync_cmdbuf.copyBuffer(entry.arena->Handle(), staging->Handle(),
                                             static_cast<u32>(entry.copies.size()), entry.copies.data());
            }
            const vk::MemoryBarrier2 after = {
                .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                .dstAccessMask = vk::AccessFlagBits2::eHostRead,
            };
            sideq_sync_cmdbuf.pipelineBarrier2(vk::DependencyInfo{
                .memoryBarrierCount = 1U,
                .pMemoryBarriers = &after,
            });
            Vulkan::Check(sideq_sync_cmdbuf.end());
            const u64 wait_value = write_tick + 1;
            const vk::Semaphore wait_semaphore = semaphore->Handle();
            const vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eAllCommands;
            const vk::TimelineSemaphoreSubmitInfo timeline_si = {
                .waitSemaphoreValueCount = 1U,
                .pWaitSemaphoreValues = &wait_value,
            };
            const vk::SubmitInfo submit_info = {
                .pNext = &timeline_si,
                .waitSemaphoreCount = 1U,
                .pWaitSemaphores = &wait_semaphore,
                .pWaitDstStageMask = &wait_stage,
                .commandBufferCount = 1U,
                .pCommandBuffers = &sideq_sync_cmdbuf,
            };
            const auto result = instance.GetSideQueue().submit(submit_info, sideq_sync_fence);
            ASSERT_MSG(result != vk::Result::eErrorDeviceLost, "Device lost during side submit");
        }
        Vulkan::Check(device.waitForFences(sideq_sync_fence, true, std::numeric_limits<u64>::max()));
        Vulkan::Check(device.resetFences(sideq_sync_fence));
        Gow3Stats::uc3_rb_side.fetch_add(1, std::memory_order_relaxed);
        Gow3Stats::uc3_rb_side_ns.fetch_add(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                 start)
                .count(),
            std::memory_order_relaxed);
        return true;
    }
    if (!side_pool) {
        side_pool = Vulkan::Check<"create readback command pool">(device.createCommandPool({
            .flags = vk::CommandPoolCreateFlagBits::eTransient |
                     vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
            .queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex(),
        }));
        const vk::CommandBufferAllocateInfo alloc_info = {
            .commandPool = side_pool,
            .level = vk::CommandBufferLevel::ePrimary,
            .commandBufferCount = 1U,
        };
        Vulkan::Check<"allocate readback command buffer">(
            device.allocateCommandBuffers(&alloc_info, &side_cmdbuf));
        side_fence = Vulkan::Check<"create readback fence">(device.createFence({}));
    }
    const auto start = std::chrono::steady_clock::now();
    Vulkan::Check(side_cmdbuf.reset());
    Vulkan::Check(side_cmdbuf.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}));
    const vk::MemoryBarrier2 before = {
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
    };
    side_cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1U,
        .pMemoryBarriers = &before,
    });
    for (const auto& entry : work) {
        side_cmdbuf.copyBuffer(entry.arena->Handle(), staging->Handle(),
                               static_cast<u32>(entry.copies.size()), entry.copies.data());
    }
    const vk::MemoryBarrier2 after = {
        .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
    };
    side_cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1U,
        .pMemoryBarriers = &after,
    });
    Vulkan::Check(side_cmdbuf.end());
    scheduler.SubmitSide(side_cmdbuf, side_fence);
    Vulkan::Check(device.waitForFences(side_fence, true, std::numeric_limits<u64>::max()));
    Vulkan::Check(device.resetFences(side_fence));
    Gow3Stats::uc3_rb_side.fetch_add(1, std::memory_order_relaxed);
    Gow3Stats::uc3_rb_side_ns.fetch_add(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                             start)
            .count(),
        std::memory_order_relaxed);
    return true;
}

// uc3: where the last GPU write to a faulting window stands (see Gow3Stats::uc3_rb_*);
// GOW3_READBACK_TRACE=1 also prints the first 200 faults.
void BufferCache::ClassifyFault(VAddr window_start) {
    const VAddr window = window_start >> WriteTickShift;
    const auto& slot = write_ticks[window % write_ticks.size()];
    const u64 write_tick =
        slot.window.load(std::memory_order_relaxed) == window ? slot.tick.load(std::memory_order_relaxed)
                                                              : 0;
    const u64 current = scheduler.CurrentTick();
    auto* semaphore = scheduler.GetWorkSemaphore();
    semaphore->Refresh();
    const u64 gpu = semaphore->KnownGpuTick();
    if (write_tick == 0) {
        Gow3Stats::uc3_rb_unknown.fetch_add(1, std::memory_order_relaxed);
    } else if (write_tick >= current) {
        Gow3Stats::uc3_rb_open.fetch_add(1, std::memory_order_relaxed);
    } else if (write_tick > gpu) {
        Gow3Stats::uc3_rb_inflight.fetch_add(1, std::memory_order_relaxed);
        Gow3Stats::uc3_rb_inflight_ticks.fetch_add(current - write_tick, std::memory_order_relaxed);
    } else {
        Gow3Stats::uc3_rb_done.fetch_add(1, std::memory_order_relaxed);
    }
    static const bool trace = EmulatorSettingsImpl::Flag("GOW3_READBACK_TRACE", false);
    static std::atomic<int> traced{0};
    if (trace && traced.fetch_add(1, std::memory_order_relaxed) < 200) {
        std::printf("readback fault: window=0x%llx write_tick=%llu current_tick=%llu gpu_tick=%llu "
                    "frame=%llu\n",
                    (unsigned long long)window_start, (unsigned long long)write_tick,
                    (unsigned long long)current, (unsigned long long)gpu,
                    (unsigned long long)frame_index);
    }
}

// uc3: GOW3_READBACK_ASYNC=1 — a read fault no longer drains the GPU from the GPU thread
// (scheduler.Finish() left the GPU idle while that thread waited, then while it recorded
// again: ~34 of ~50 ms per frame in the streets). See AsyncReadback.
static bool ReadbackAsync() {
    static const bool async = EmulatorSettings.GetReadbacksMode() == GpuReadbacksMode::Precise &&
                              EmulatorSettingsImpl::Flag("GOW3_READBACK_ASYNC", false);
    return async;
}

static bool ReadbackDoneFast() {
    static const bool fast =
        ReadbackAsync() && EmulatorSettingsImpl::Flag("GOW3_READBACK_DONE_FAST", false);
    return fast;
}

std::unique_lock<std::recursive_mutex> BufferCache::RangesLock() {
    if (!ReadbackDoneFast()) {
        return {};
    }
    return std::unique_lock{ranges_mutex};
}

// uc3 (Fable R.14 D4 diagnostics): why FastDoneReadback refused (1 not ready, 2 no side
// queue, 3 ReadbackAll or Hot, 4 arena mismatch, 5 window over a slot, 6 no free slot,
// 7 overlaps a pending readback, 8 nothing or too much to copy, 9 side queue copy failed,
// 10 served; 0 not tried).
thread_local int fast_reason = 0;

std::string BufferCache::DescribeGpuRanges(VAddr addr, u64 size) {
    std::string out;
    int n = 0;
    const auto ranges_lock = RangesLock();
    gpu_modified_ranges.ForEachInRange(addr, size, [&](VAddr start, VAddr end) {
        if (n++ < 6) {
            out += fmt::format(" [{:x}, +{})", start, end - start);
        }
    });
    if (n > 6) {
        out += fmt::format(" (+{} more)", n - 6);
    }
    return out.empty() ? " none" : out;
}

bool BufferCache::FastDoneReadback(VAddr device_addr, u64 size, bool is_write) {
    if (!fast_ready.load(std::memory_order_acquire) || !instance.GetSideQueue() ||
        ReadbackAll() || ReadbackHot()) {
        fast_reason = !fast_ready.load(std::memory_order_acquire) ? 1
                      : !instance.GetSideQueue()                  ? 2
                                                                  : 3;
        return false;
    }
    constexpr u64 WindowSize = 512_KB;
    const u64 first_block = device_addr >> block_shift;
    const u64 last_block = (device_addr + size - 1) >> block_shift;
    const u64 first_page = first_block >> blocks_per_arena_page_shift;
    const u64 last_page = last_block >> blocks_per_arena_page_shift;
    const Buffer* arena = address_space[first_page];
    if (!arena || address_space[last_page] != arena) {
        fast_reason = 4;
        return false;
    }
    const VAddr arena_end = arena->cpu_addr + arena->size_bytes;
    const VAddr window_start =
        std::max<VAddr>(Common::AlignDown(device_addr, WindowSize), arena->cpu_addr);
    const VAddr window_end = std::min<VAddr>(
        std::max<VAddr>(window_start + WindowSize, device_addr + size), arena_end);
    if (window_end - window_start > FastSlotSize) {
        fast_reason = 5;
        return false;
    }
    u32 slot;
    {
        std::scoped_lock lk{fast_mutex};
        if (fast_free_slots.empty()) {
            fast_reason = 6;
            return false;
        }
        slot = fast_free_slots.back();
        fast_free_slots.pop_back();
    }
    const auto give_back = [&] {
        std::scoped_lock lk{fast_mutex};
        fast_free_slots.push_back(slot);
    };
    auto readback = std::make_shared<AsyncReadback>();
    readback->fast_slot = slot;
    readback->download = fast_staging;
    readback->download.offset = fast_staging.offset + u64{slot} * FastSlotSize;
    readback->download.size = FastSlotSize;
    readback->download.mapped = fast_staging.mapped + u64{slot} * FastSlotSize;
    readback->windows.push_back({arena, window_start, window_end - window_start});
    {
        const auto ranges_lock = RangesLock();
        // A window still landing from another readback: the usual path waits for it.
        {
            std::scoped_lock lk{async_mutex};
            for (const auto& entry : async_pending) {
                for (const auto& w : entry->windows) {
                    if (w.start < window_end && window_start < w.start + w.size) {
                        give_back();
                        fast_reason = 7;
                        return false;
                    }
                }
            }
        }
        boost::container::small_vector<vk::BufferCopy, 8> copies;
        u64 total_size_bytes = 0;
        const VAddr arena_base = arena->cpu_addr;
        memory_tracker->ForEachDownloadRange<false>(
            window_start, window_end - window_start, [&](u64 address, u64 range_size) {
                gpu_modified_ranges.ForEachInRange(address, range_size, [&](VAddr start, VAddr end) {
                    copies.push_back(vk::BufferCopy{
                        .srcOffset = start - arena_base,
                        .dstOffset = total_size_bytes,
                        .size = end - start,
                    });
                    constexpr u64 align = 64ULL;
                    total_size_bytes += (end - start + align - 1) & ~(align - 1);
                });
            });
        if (total_size_bytes == 0 || total_size_bytes > FastSlotSize) {
            give_back();
            fast_reason = 8;
            return false;
        }
        for (auto& copy : copies) {
            readback->copies.push_back(
                {arena_base + copy.srcOffset, copy.dstOffset, copy.size});
            copy.dstOffset += readback->download.offset;
        }
        // Only windows whose last write (and the tick after it) has finished: checked under
        // ranges_mutex, so stage B cannot bind a new write in between.
        const std::pair<const Buffer*, std::span<const vk::BufferCopy>> work[] = {
            {arena, std::span<const vk::BufferCopy>(copies.data(), copies.size())}};
        if (!SideQueueCopy(*readback, work)) {
            give_back();
            fast_reason = 9;
            return false;
        }
        ClassifyFault(Common::AlignDown(device_addr, WindowSize));
        memory_tracker->ForEachDownloadRange<false>(
            window_start, window_end - window_start,
            [&](u64 address, u64 range_size) { gpu_modified_ranges.Subtract(address, range_size); });
        readback->tick = scheduler.CurrentTick();
        std::scoped_lock lk{async_mutex};
        async_pending.push_back(readback);
        async_count.fetch_add(1, std::memory_order_release);
    }
    Gow3Stats::uc3_rb_side.fetch_add(1, std::memory_order_relaxed);
    Gow3Stats::uc3_rb_async.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<u32> fast_count{0};
    const u32 n = fast_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 2 || (n & 4095) == 0) {
        std::printf("uc3: done-window readbacks on the faulting thread: %u\n", n);
    }
    CompleteAsyncReadback(*readback, device_addr, size, is_write);
    fast_reason = 10;
    return true;
}

void BufferCache::ReadMemory(VAddr device_addr, u64 size, bool is_write, bool assume_locks,
                             bool from_fault) {
    struct FaultDiag {
        int category = -1; // 0 unknown, 1 open, 2 in flight, 3 done
        bool false_dep = false;
        VAddr addr = 0;
        u64 size = 0;
        u64 writer = 0;
        bool from_page = false;
        std::string info; // R.14 D4 diagnostics: own bytes GPU-written, page's GPU ranges
        std::chrono::steady_clock::time_point start;
        std::chrono::steady_clock::time_point cp_start;
        std::chrono::steady_clock::time_point cp_end;
        std::chrono::steady_clock::time_point land_start;
        std::chrono::steady_clock::time_point land_end;
        ~FaultDiag() {
            if (category < 0) {
                return;
            }
            const auto now = std::chrono::steady_clock::now();
            const u64 ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - start).count();
            diag_totals.count[category].fetch_add(1, std::memory_order_relaxed);
            diag_totals.ns[category].fetch_add(ns, std::memory_order_relaxed);
            if (auto* thread = DiagThisThread()) {
                thread->count.fetch_add(1, std::memory_order_relaxed);
                thread->ns.fetch_add(ns, std::memory_order_relaxed);
            }
            if (Gow3Stats::d4_diag) {
                const int kind = DiagWatchedThread();
                Gow3Stats::fast_reason[kind < 0 ? 2 : kind][fast_reason].fetch_add(
                    1, std::memory_order_relaxed);
            }
            if (const int kind = DiagWatchedThread(); kind >= 0) {
                std::scoped_lock lock{diag_writer_mutex};
                auto& row = diag_writers[kind][writer];
                ++row.count;
                row.ns += ns;
                if (diag_samples[kind].size() < 6) {
                    diag_samples[kind].push_back({addr, size, category, writer, from_page, info});
                }
            }
            const auto span = [](auto a, auto b) -> u64 {
                return a.time_since_epoch().count() && b > a
                           ? std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count()
                           : 0;
            };
            diag_totals.queue_ns[category].fetch_add(span(start, cp_start),
                                                     std::memory_order_relaxed);
            diag_totals.cp_ns[category].fetch_add(span(cp_start, cp_end), std::memory_order_relaxed);
            diag_totals.land_ns[category].fetch_add(span(land_start, land_end),
                                                    std::memory_order_relaxed);
            if (false_dep) {
                diag_totals.false_count.fetch_add(1, std::memory_order_relaxed);
                diag_totals.false_ns.fetch_add(ns, std::memory_order_relaxed);
            }
            const u64 now_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
            u64 last = diag_totals.last_print_ns.load(std::memory_order_relaxed);
            if (now_ns - last < 5000000000ULL ||
                !diag_totals.last_print_ns.compare_exchange_strong(last, now_ns)) {
                return;
            }
            u64 c[4], t[4], q[4], cp[4], l[4];
            for (int i = 0; i < 4; ++i) {
                c[i] = diag_totals.count[i].exchange(0, std::memory_order_relaxed);
                t[i] = diag_totals.ns[i].exchange(0, std::memory_order_relaxed);
                q[i] = diag_totals.queue_ns[i].exchange(0, std::memory_order_relaxed);
                cp[i] = diag_totals.cp_ns[i].exchange(0, std::memory_order_relaxed);
                l[i] = diag_totals.land_ns[i].exchange(0, std::memory_order_relaxed);
            }
            const u64 fc = diag_totals.false_count.exchange(0, std::memory_order_relaxed);
            const u64 ft = diag_totals.false_ns.exchange(0, std::memory_order_relaxed);
            const u64 pu = diag_totals.page_unknown.exchange(0, std::memory_order_relaxed);
            const auto line = [&](int i) {
                return fmt::format("{} faults {:.1f} ms (avg {:.2f} ms: queue {:.2f}, on CP {:.2f}, "
                                   "copy landing {:.2f})",
                                   c[i], t[i] / 1e6, c[i] ? t[i] / 1e6 / c[i] : 0.0,
                                   c[i] ? q[i] / 1e6 / c[i] : 0.0, c[i] ? cp[i] / 1e6 / c[i] : 0.0,
                                   c[i] ? l[i] / 1e6 / c[i] : 0.0);
            };
            std::printf("uc3 fault categories (5 s): open %s; in flight %s; done %s; unknown %s; "
                        "false dependency (page written before the window's last write, page done) "
                        "%llu faults %.1f ms; page tick unknown %llu\n",
                        line(1).c_str(), line(2).c_str(), line(3).c_str(), line(0).c_str(),
                        (unsigned long long)fc, ft / 1e6, (unsigned long long)pu);
            struct Row {
                const char* name;
                u64 count;
                u64 ns;
            };
            std::vector<Row> rows;
            for (auto& slot : diag_threads) {
                if (slot.used.load(std::memory_order_acquire)) {
                    rows.push_back({slot.name, slot.count.exchange(0, std::memory_order_relaxed),
                                    slot.ns.exchange(0, std::memory_order_relaxed)});
                }
            }
            std::ranges::sort(rows, [](const Row& a, const Row& b) { return a.ns > b.ns; });
            {
                std::scoped_lock lock{diag_writer_mutex};
                static constexpr const char* kinds[2] = {"GfxEop handler", "NdJobWorkerThread 0"};
                for (int k = 0; k < 2; ++k) {
                    std::printf("uc3 fault writers %s (5 s; writer: faults, ms; 1 = a draw, "
                                "small = dispatch hash, big = task function):",
                                kinds[k]);
                    for (const auto& [w, row] : diag_writers[k]) {
                        std::printf(" %llx: %llu, %.1f;", (unsigned long long)w,
                                    (unsigned long long)row.count, row.ns / 1e6);
                    }
                    std::printf(" samples:");
                    for (const auto& s : diag_samples[k]) {
                        std::printf(" [%010llx +%llu cat %d writer %llx%s%s]",
                                    (unsigned long long)s.addr, (unsigned long long)s.size,
                                    s.category, (unsigned long long)s.writer,
                                    s.from_page ? "" : " (window)", s.info.c_str());
                    }
                    std::printf("\n");
                    diag_writers[k].clear();
                    diag_samples[k].clear();
                }
            }
            if (Gow3Stats::d4_diag) {
                static constexpr const char* who[3] = {"GfxEop handler", "NdJobWorkerThread 0",
                                                       "others"};
                std::printf("uc3 D4 fast readback reasons (5 s; 0 not tried, 1 not ready, 2 no "
                            "side queue, 3 all/hot, 4 arena, 5 window > slot, 6 no slot, "
                            "7 pending overlap, 8 copy size, 9 side copy failed, 10 served):");
                for (int k = 0; k < 3; ++k) {
                    std::printf(" %s", who[k]);
                    for (int r = 0; r < 11; ++r) {
                        if (const u64 n = Gow3Stats::fast_reason[k][r].exchange(
                                0, std::memory_order_relaxed)) {
                            std::printf(" r%d=%llu", r, (unsigned long long)n);
                        }
                    }
                    std::printf(";");
                }
                std::printf("\n");
            }
            std::printf("uc3 fault threads (5 s, faults, ms waited):");
            for (size_t i = 0; i < rows.size() && i < 5; ++i) {
                std::printf(" %s %llu %.1f;", rows[i].name, (unsigned long long)rows[i].count,
                            rows[i].ns / 1e6);
            }
            std::printf("\n");
        }
    } fault_diag;
    if (from_fault && FaultCategoryDiag()) {
        fault_diag.start = std::chrono::steady_clock::now();
        fault_diag.addr = device_addr;
        fault_diag.size = size;
        fault_diag.writer = DiagWriterOf(device_addr, fault_diag.from_page);
        fast_reason = 0;
        if (Gow3Stats::d4_diag && DiagWatchedThread() >= 0) {
            fault_diag.info = fmt::format(
                " own bytes {}; page ranges{}",
                IsGpuModifiedBytes(device_addr, std::max<u64>(size, 1)) ? "GPU" : "clean",
                DescribeGpuRanges(Common::AlignDown(device_addr, 4096), 4096));
        }
        const VAddr window = device_addr >> WriteTickShift;
        if (!DiagWindowFaulted(window)) {
            diag_fault_windows[diag_fault_window_next.fetch_add(1, std::memory_order_relaxed) %
                               diag_fault_windows.size()]
                .store(window + 1, std::memory_order_relaxed);
        }
        const auto& slot = write_ticks[window % write_ticks.size()];
        const u64 window_tick = slot.window.load(std::memory_order_relaxed) == window
                                    ? slot.tick.load(std::memory_order_relaxed)
                                    : 0;
        const u64 current = scheduler.CurrentTick();
        const u64 gpu = scheduler.GetWorkSemaphore()->KnownGpuTick();
        fault_diag.category = window_tick == 0         ? 0
                              : window_tick >= current ? 1
                              : window_tick > gpu      ? 2
                                                       : 3;
        u64 page_tick = 0;
        bool unknown = false;
        for (VAddr page = device_addr >> DiagPageShift;
             page <= (device_addr + std::max<u64>(size, 1) - 1) >> DiagPageShift; ++page) {
            const u64 tick = DiagPageTick(page);
            unknown |= tick == 0;
            page_tick = std::max(page_tick, tick);
        }
        if (unknown) {
            diag_totals.page_unknown.fetch_add(1, std::memory_order_relaxed);
        } else if (fault_diag.category == 1 || fault_diag.category == 2) {
            fault_diag.false_dep = page_tick < window_tick && page_tick <= gpu;
        }
    }
    bool async = from_fault && !assume_locks && ReadbackAsync();
    if (async && ReadbackAcbSync()) {
        // uc3: windows an async compute dispatch writes take the synchronous path: the command
        // processor stays blocked instead of running ahead into the next frame's compute work
        // (UC2 train: Drake fell behind with READBACK_ASYNC + COMPUTE_PIPE).
        const VAddr window = device_addr >> WriteTickShift;
        if (acb_windows[window % acb_windows.size()].load(std::memory_order_relaxed) == window + 1) {
            async = false;
            static std::atomic<u32> acb_sync{0};
            const u32 n = acb_sync.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 3 || (n & 1023) == 0) {
                std::printf("uc3: async compute window read synchronously: %u\n", n);
            }
        }
    }
    if (!async) {
        (assume_locks ? Gow3Stats::uc3_rb_sync_gpu
         : is_write   ? Gow3Stats::uc3_rb_sync_write
                      : Gow3Stats::uc3_rb_sync_other)
            .fetch_add(1, std::memory_order_relaxed);
    }
    if (async && ReadbackDoneFast()) {
        if (fault_diag.category >= 0) {
            fault_diag.land_start = std::chrono::steady_clock::now();
        }
        if (FastDoneReadback(device_addr, size, is_write)) {
            if (fault_diag.category >= 0) {
                fault_diag.land_end = std::chrono::steady_clock::now();
            }
            return;
        }
        fault_diag.land_start = {};
    }
    std::shared_ptr<AsyncReadback> pending;
    const auto flush_request = [this, device_addr, size, is_write, async, &pending, &fault_diag] {
        Gow3Stats::Timer flush_timer{Gow3Stats::flush_ns};
        Gow3Stats::flush_count.fetch_add(1, std::memory_order_relaxed);
        struct CpSpan {
            decltype(fault_diag)& diag;
            explicit CpSpan(decltype(fault_diag)& d) : diag{d} {
                if (diag.category >= 0) {
                    diag.cp_start = std::chrono::steady_clock::now();
                }
            }
            ~CpSpan() {
                if (diag.category >= 0) {
                    diag.cp_end = std::chrono::steady_clock::now();
                }
            }
        } cp_span{fault_diag};
        if (ReadbackDoneFast() && !fast_ready.load(std::memory_order_relaxed)) {
            fast_staging = staging_pool.Request(u64{FastSlotSize} * FastSlots,
                                                VideoCore::MemoryType::HostCached, 0, true);
            {
                std::scoped_lock lk{fast_mutex};
                for (u32 i = FastSlots; i-- > 0;) {
                    fast_free_slots.push_back(i);
                }
            }
            fast_ready.store(true, std::memory_order_release);
        }
        // uc3 (GOW3_READBACK_ASYNC_DRAIN=1): the drain in Rasterizer::ReadMemory runs on the
        // faulting thread, where it does nothing. Work still queued for the recording thread
        // (GOW3_COMPUTE_PIPE: async compute dispatches) is recorded after this copy and its
        // writes are marked later; the UC2 train carriage fell early with READBACK_ASYNC +
        // COMPUTE_PIPE + the scalar copy HLE. Drain here, on the command processor thread.
        static const bool async_drain = EmulatorSettingsImpl::Flag("GOW3_READBACK_ASYNC_DRAIN", false);
        if (async && async_drain) {
            if (auto* rasterizer = liverpool->GetRasterizer()) {
                static std::atomic<u32> drains{0};
                const u32 n = drains.fetch_add(1, std::memory_order_relaxed) + 1;
                if (n <= 2 || (n & 4095) == 0) {
                    std::printf("uc3: async readback drains: %u\n", n);
                }
                rasterizer->DrainDrawPipe(Vulkan::DrawPipe::ReasonRasterizer);
            }
        }
        if (ReadbackAll()) {
            WaitAsyncReadbacks(0, 0);
            DownloadAllGpuModified();
            if (is_write) {
                memory_tracker->MarkRegionAsCpuModified(device_addr, size);
            }
            return;
        }
        const u32 first_block = device_addr >> block_shift;
        const u32 last_block = (device_addr + size - 1) >> block_shift;
        const auto* arena = GetArena(first_block, last_block);

        // GPU-modified ranges come as many small scattered islands,
        // so the download is widened to a window around the request
        constexpr u64 WindowSize = 512_KB;
        const VAddr arena_end = arena->cpu_addr + arena->size_bytes;
        const VAddr window_start =
            std::max<VAddr>(Common::AlignDown(device_addr, WindowSize), arena->cpu_addr);
        const VAddr window_end = std::min<VAddr>(
            std::max<VAddr>(window_start + WindowSize, device_addr + size), arena_end);
        ClassifyFault(Common::AlignDown(device_addr, WindowSize));
        boost::container::small_vector<DownloadWindow, 17> windows;
        windows.push_back({arena, window_start, window_end - window_start});
        if (ReadbackHot()) {
            boost::container::small_vector<VAddr, 16> others;
            NoteHotWindow(window_start, others);
            for (const VAddr start : others) {
                const auto* other = GetArena(start >> block_shift,
                                             (start + WindowSize - 1) >> block_shift);
                const VAddr begin = std::max<VAddr>(start, other->cpu_addr);
                const VAddr end = std::min<VAddr>(start + WindowSize,
                                                  other->cpu_addr + other->size_bytes);
                if (end <= begin || !memory_tracker->IsRegionGpuModified(begin, end - begin)) {
                    continue;
                }
                windows.push_back({other, begin, end - begin});
            }
            Gow3Stats::uc3_readback_hot.fetch_add(1, std::memory_order_relaxed);
            Gow3Stats::uc3_readback_hot_windows.fetch_add(windows.size() - 1,
                                                          std::memory_order_relaxed);
        }
        // A window still being read back asynchronously: its ranges are no longer in
        // gpu_modified_ranges, so let that readback land first.
        {
            Gow3Stats::Timer wait_timer{Gow3Stats::flush_wait_ns};
            for (const auto& window : windows) {
                WaitAsyncReadbacks(window.start, window.start + window.size);
            }
        }
        Gow3Stats::Timer download_timer{Gow3Stats::flush_download_ns};
        if (async) {
            // uc3 (Drake's scarf, diagnosis): a side-queue readback lets the guest go on before
            // the work recorded so far runs; the graphics path waited for all of it, deferred
            // host copies of guest memory included. GOW3_SIDEQ_HOST_COPIES=1 finishes those
            // copies first (as before a guest-visible write); 2 also the pending image downloads.
            static const int host_copies = [] {
                const char* env = std::getenv("GOW3_SIDEQ_HOST_COPIES");
                return env ? std::atoi(env) : 0;
            }();
            if (host_copies > 0) {
                if (auto* rasterizer = liverpool->GetRasterizer()) {
                    rasterizer->WaitHostCopies();
                    if (host_copies > 1) {
                        rasterizer->ProcessDownloadImages();
                    }
                }
            }
            // The faulting thread marks the written range itself, after the data lands.
            pending = DownloadWindowsAsync(windows);
            if (pending) {
                return;
            }
        } else if (windows.size() > 1) {
            DownloadWindows(windows);
        } else {
            DownloadMemory(arena, window_start, window_end - window_start);
        }
        if (is_write) {
            memory_tracker->MarkRegionAsCpuModified(device_addr, size);
        }
    };
    if (assume_locks) {
        flush_request();
    } else {
        liverpool->SendCommand<true>(std::move(flush_request));
    }
    if (pending) {
        if (fault_diag.category >= 0) {
            fault_diag.land_start = std::chrono::steady_clock::now();
        }
        CompleteAsyncReadback(*pending, device_addr, size, is_write);
        if (fault_diag.category >= 0) {
            fault_diag.land_end = std::chrono::steady_clock::now();
        }
        // uc3 (GOW3_READBACK_DIRTY_RETRY=1): the GPU wrote the window after this copy was
        // recorded, so the bytes that landed are an older generation (UC2 train: Drake fell
        // behind with READBACK_ASYNC + COMPUTE_PIPE). Read it again, synchronously.
        static const bool dirty_retry = EmulatorSettingsImpl::Flag("GOW3_READBACK_DIRTY_RETRY", false);
        bool dirty;
        {
            std::scoped_lock lk{async_mutex};
            dirty = pending->dirty;
        }
        if (dirty_retry && dirty) {
            static std::atomic<u32> retries{0};
            const u32 n = retries.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 3 || (n & 63) == 0) {
                std::printf("uc3: dirty async readback read again: %u\n", n);
            }
            ReadMemory(device_addr, size, is_write, false, false);
        }
    }
}

// uc3 (Drake's scarf, diagnosis): GOW3_SIDEQ_VERIFY=1 also copies side-queue readbacks on the
// graphics queue and compares both on landing. The game still gets the side-queue bytes, but
// only once the graphics copy has landed (the timing of the path without the side queue).
static bool SideqVerify() {
    static const bool enabled = EmulatorSettingsImpl::Flag("GOW3_SIDEQ_VERIFY", false);
    return enabled;
}

std::shared_ptr<BufferCache::AsyncReadback> BufferCache::DownloadWindowsAsync(
    std::span<const DownloadWindow> windows) {
    const auto ranges_lock = RangesLock();
    struct WindowCopies {
        const DownloadWindow* window;
        boost::container::small_vector<vk::BufferCopy, 4> copies;
    };
    boost::container::small_vector<WindowCopies, 17> work;
    u64 total_size_bytes = 0;
    for (const auto& window : windows) {
        WindowCopies entry{&window};
        const VAddr arena_base = window.arena->cpu_addr;
        memory_tracker->ForEachDownloadRange<false>(
            window.start, window.size, [&](u64 address, u64 size) {
                gpu_modified_ranges.ForEachInRange(address, size, [&](VAddr start, VAddr end) {
                    entry.copies.push_back(vk::BufferCopy{
                        .srcOffset = start - arena_base,
                        .dstOffset = total_size_bytes,
                        .size = end - start,
                    });
                    constexpr u64 align = 64ULL;
                    total_size_bytes += (end - start + align - 1) & ~(align - 1);
                });
                gpu_modified_ranges.Subtract(address, size);
            });
        if (!entry.copies.empty()) {
            work.push_back(std::move(entry));
        }
    }
    if (total_size_bytes == 0) {
        return nullptr;
    }
    auto readback = std::make_shared<AsyncReadback>();
    readback->download =
        staging_pool.Request(total_size_bytes, VideoCore::MemoryType::HostCached, 0, true);
    for (auto& entry : work) {
        const VAddr arena_base = entry.window->arena->cpu_addr;
        for (auto& copy : entry.copies) {
            readback->copies.push_back({arena_base + copy.srcOffset, copy.dstOffset, copy.size});
            copy.dstOffset += readback->download.offset;
        }
    }
    // gow3 R.9.6: windows whose last GPU write (and the tick after it, see SideDownload) has
    // finished are copied on the side queue: no wait behind the graphics queue's work.
    readback->windows.assign(windows.begin(), windows.end());
    bool side = false;
    if (instance.GetSideQueue()) {
        boost::container::small_vector<std::pair<const Buffer*, std::span<const vk::BufferCopy>>, 17>
            side_work;
        for (const auto& entry : work) {
            side_work.emplace_back(entry.window->arena,
                                   std::span<const vk::BufferCopy>(entry.copies.data(),
                                                                   entry.copies.size()));
        }
        side = SideQueueCopy(*readback, side_work);
    }
    if (!side) {
        for (auto& entry : work) {
            runtime.CopyBuffer(entry.window->arena, readback->download.buffer, entry.copies);
        }
    }
    readback->tick = scheduler.CurrentTick();
    {
        std::scoped_lock lk{async_mutex};
        async_pending.push_back(readback);
    }
    async_count.fetch_add(1, std::memory_order_release);
    // uc3 (Drake's scarf, diagnosis): GOW3_SIDEQ_WAIT_GFX=1 makes a side-queue readback land
    // only once the graphics work recorded so far has run (the verify wait, without its copy).
    // GOW3_SIDEQ_LOG_WINDOWS=1 prints the first side-queue windows (address, size).
    static const bool wait_gfx = EmulatorSettingsImpl::Flag("GOW3_SIDEQ_WAIT_GFX", false);
    static const bool log_windows = EmulatorSettingsImpl::Flag("GOW3_SIDEQ_LOG_WINDOWS", false);
    if (side && log_windows) {
        static std::atomic<u32> logged{0};
        const u32 n = logged.fetch_add(1, std::memory_order_relaxed);
        if (n < 400) {
            for (const auto& window : readback->windows) {
                std::printf("uc3: side queue window %#llx+%#llx (%zu copies)\n",
                            (unsigned long long)window.start, (unsigned long long)window.size,
                            readback->copies.size());
            }
        }
    }
    // GOW3_SIDEQ_WAIT_RANGE=lo-hi[,lo-hi...] (hex): the same wait, only for windows that touch
    // one of these ranges (bisection of which guest data races).
    static const std::vector<std::pair<VAddr, VAddr>> wait_ranges = [] {
        std::vector<std::pair<VAddr, VAddr>> ranges;
        const char* env = std::getenv("GOW3_SIDEQ_WAIT_RANGE");
        while (env && *env) {
            char* end = nullptr;
            const VAddr lo = std::strtoull(env, &end, 16);
            if (!end || *end != '-') {
                break;
            }
            const VAddr hi = std::strtoull(end + 1, &end, 16);
            ranges.emplace_back(lo, hi);
            std::printf("uc3: side queue wait range %#llx-%#llx\n", (unsigned long long)lo,
                        (unsigned long long)hi);
            env = end && *end == ',' ? end + 1 : nullptr;
        }
        return ranges;
    }();
    bool range_hit = false;
    if (side && !wait_ranges.empty()) {
        for (const auto& window : readback->windows) {
            for (const auto& [lo, hi] : wait_ranges) {
                range_hit |= window.start < hi && lo < window.start + window.size;
            }
        }
        if (range_hit) {
            static std::atomic<u32> hits{0};
            const u32 n = hits.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 2 || (n & 1023) == 0) {
                std::printf("uc3: side queue wait range hits: %u\n", n);
            }
        }
    }
    if (side && (wait_gfx || range_hit) && !SideqVerify()) {
        readback->verify_tick = scheduler.CurrentTick();
        readback->has_wait_gfx = true;
        scheduler.Flush();
    }
    if (side && SideqVerify()) {
        readback->verify =
            staging_pool.Request(total_size_bytes, VideoCore::MemoryType::HostCached, 0, true);
        for (const auto& entry : work) {
            auto copies = entry.copies;
            for (auto& copy : copies) {
                copy.dstOffset = copy.dstOffset - readback->download.offset + readback->verify.offset;
            }
            runtime.CopyBuffer(entry.window->arena, readback->verify.buffer, copies);
        }
        readback->verify_tick = scheduler.CurrentTick();
        readback->has_verify = true;
        scheduler.Flush();
    }
    if (side) {
        Gow3Stats::uc3_rb_side.fetch_add(1, std::memory_order_relaxed);
        Gow3Stats::uc3_rb_async.fetch_add(1, std::memory_order_relaxed);
        return readback;
    }
    scheduler.Flush();
    Gow3Stats::uc3_rb_async.fetch_add(1, std::memory_order_relaxed);
    return readback;
}

bool BufferCache::SideQueueCopy(
    AsyncReadback& readback,
    std::span<const std::pair<const Buffer*, std::span<const vk::BufferCopy>>> work) {
    u64 write_tick = 0;
    for (const auto& window : readback.windows) {
        const u64 tick = LastWriteTick(window.start, window.size);
        if (tick == ~0ULL) {
            return false;
        }
        write_tick = std::max(write_tick, tick);
    }
    // uc3 (Drake's scarf, diagnosis): GOW3_SIDEQ_MARGIN=N requires the N ticks after the noted
    // write tick to be finished (default 1). If a larger margin fixes the frozen cloth, the noted
    // tick can lag the submission that really holds the write.
    static const u64 margin = [] {
        const char* env = std::getenv("GOW3_SIDEQ_MARGIN");
        const long value = env ? std::atol(env) : 1;
        return u64(value < 1 ? 1 : value);
    }();
    auto* semaphore = scheduler.GetWorkSemaphore();
    if (semaphore->KnownGpuTick() < write_tick + margin) {
        semaphore->Refresh();
        if (semaphore->KnownGpuTick() < write_tick + margin) {
            static const bool log = EmulatorSettingsImpl::Flag("GOW3_SIDEQ_ARENA_LOG", false);
            if (log) {
                static std::atomic<u32> refused{0};
                const u32 n = refused.fetch_add(1, std::memory_order_relaxed) + 1;
                if (n <= 4 || (n & 255) == 0) {
                    std::printf("uc3: side queue copies refused (write not landed): %u\n", n);
                }
            }
            return false;
        }
    }
    const vk::Device device = instance.GetDevice();
    std::scoped_lock lk{sideq_mutex};
    if (!sideq_pool) {
        sideq_pool = Vulkan::Check<"create side queue command pool">(device.createCommandPool({
            .flags = vk::CommandPoolCreateFlagBits::eTransient |
                     vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
            .queueFamilyIndex = instance.GetSideQueueFamilyIndex(),
        }));
    }
    u32 index = ~0u;
    for (u32 i = 0; i < sideq_slots.size(); ++i) {
        if (!sideq_slots[i].busy) {
            index = i;
            break;
        }
    }
    if (index == ~0u) {
        return false;
    }
    auto& slot = sideq_slots[index];
    if (!slot.cmdbuf) {
        const vk::CommandBufferAllocateInfo alloc_info = {
            .commandPool = sideq_pool,
            .level = vk::CommandBufferLevel::ePrimary,
            .commandBufferCount = 1U,
        };
        Vulkan::Check<"allocate side queue command buffer">(
            device.allocateCommandBuffers(&alloc_info, &slot.cmdbuf));
        slot.fence = Vulkan::Check<"create side queue fence">(device.createFence({}));
    }
    Vulkan::Check(slot.cmdbuf.reset());
    Vulkan::Check(slot.cmdbuf.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}));
    for (const auto& [arena, copies] : work) {
        slot.cmdbuf.copyBuffer(arena->Handle(), readback.download.buffer->Handle(),
                               static_cast<u32>(copies.size()), copies.data());
    }
    const vk::MemoryBarrier2 after = {
        .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
    };
    slot.cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1U,
        .pMemoryBarriers = &after,
    });
    Vulkan::Check(slot.cmdbuf.end());
    // The finished tick carries the writes' memory dependency (already signalled: no wait).
    const u64 wait_value = write_tick + margin;
    const vk::Semaphore wait_semaphore = semaphore->Handle();
    const vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eAllCommands;
    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .waitSemaphoreValueCount = 1U,
        .pWaitSemaphoreValues = &wait_value,
    };
    const vk::SubmitInfo submit_info = {
        .pNext = &timeline_si,
        .waitSemaphoreCount = 1U,
        .pWaitSemaphores = &wait_semaphore,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1U,
        .pCommandBuffers = &slot.cmdbuf,
    };
    const auto result = instance.GetSideQueue().submit(submit_info, slot.fence);
    ASSERT_MSG(result != vk::Result::eErrorDeviceLost, "Device lost during side submit");
    slot.busy = true;
    readback.side_slot = index;
    return true;
}

void BufferCache::CompleteAsyncReadback(AsyncReadback& readback, VAddr device_addr, u64 size,
                                        bool is_write) {
    const auto start = std::chrono::steady_clock::now();
    if (readback.side_slot != ~0u) {
        auto& slot = sideq_slots[readback.side_slot];
        const vk::Device device = instance.GetDevice();
        Vulkan::Check(device.waitForFences(slot.fence, true, std::numeric_limits<u64>::max()));
        Vulkan::Check(device.resetFences(slot.fence));
        {
            std::scoped_lock lk{sideq_mutex};
            slot.busy = false;
        }
        if (readback.has_wait_gfx) {
            scheduler.GetWorkSemaphore()->Wait(readback.verify_tick);
        }
        // uc3 (Drake's scarf, diagnosis): GOW3_SIDEQ_SLEEP_US=N holds a side-queue readback
        // N microseconds before the guest goes on: a timing control with no ordering.
        static const long sleep_us = [] {
            const char* env = std::getenv("GOW3_SIDEQ_SLEEP_US");
            return env ? std::atol(env) : 0L;
        }();
        if (sleep_us > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
        }
        if (readback.has_verify) {
            scheduler.GetWorkSemaphore()->Wait(readback.verify_tick);
            readback.download.buffer->Invalidate(readback.download.offset, readback.download.size);
            readback.verify.buffer->Invalidate(readback.verify.offset, readback.verify.size);
            static std::atomic<u64> checked{0}, differing{0};
            u64 diff_bytes = 0;
            VAddr first_diff = 0;
            for (const auto& copy : readback.copies) {
                const u8* side_bytes = readback.download.mapped + copy.staging_offset;
                const u8* gfx_bytes = readback.verify.mapped + copy.staging_offset;
                for (u64 i = 0; i < copy.size; ++i) {
                    if (side_bytes[i] != gfx_bytes[i]) {
                        if (diff_bytes++ == 0) {
                            first_diff = copy.address + i;
                        }
                    }
                }
            }
            bool dirty;
            {
                std::scoped_lock lk{async_mutex};
                dirty = readback.dirty;
                async_free.push_back(readback.verify);
                async_free_count.fetch_add(1, std::memory_order_release);
            }
            const u64 n = checked.fetch_add(1, std::memory_order_relaxed) + 1;
            if (diff_bytes) {
                const u64 d = differing.fetch_add(1, std::memory_order_relaxed) + 1;
                if (d <= 40 || (d & 63) == 0) {
                    std::printf("uc3: side queue verify DIFF %llu/%llu: %llu bytes differ, first at "
                                "%#llx, window %#llx+%#llx, dirty %d\n",
                                (unsigned long long)d, (unsigned long long)n,
                                (unsigned long long)diff_bytes, (unsigned long long)first_diff,
                                (unsigned long long)readback.windows[0].start,
                                (unsigned long long)readback.windows[0].size, int(dirty));
                }
            } else if (n <= 2 || (n & 1023) == 0) {
                std::printf("uc3: side queue verify: %llu checked, %llu differing\n",
                            (unsigned long long)n,
                            (unsigned long long)differing.load(std::memory_order_relaxed));
            }
        }
    } else {
        scheduler.GetWorkSemaphore()->Wait(readback.tick);
    }
    const auto& download = readback.download;
    download.buffer->Invalidate(download.offset, download.size);
    if (ReadbackLabelGuard()) {
        // Labels written since this copy was recorded keep their newer bytes: the copy lands
        // around them (under async_mutex, so a label noted later finds the entry gone).
        std::scoped_lock lk{async_mutex};
        for (const auto& copy : readback.copies) {
            const u8* src = download.mapped + copy.staging_offset;
            Gow3WriteLog::NoteDownload(copy.address, src, copy.size);
            VAddr cursor = copy.address;
            const VAddr end = copy.address + copy.size;
            std::vector<std::pair<VAddr, u64>> holes;
            for (const auto& [address, size] : readback.keep) {
                if (address < end && cursor < address + size) {
                    holes.emplace_back(std::max(address, copy.address),
                                       std::min(address + size, end));
                }
            }
            std::ranges::sort(holes);
            for (const auto& [begin, hole_end] : holes) {
                if (begin > cursor) {
                    memory->TryWriteBacking(std::bit_cast<u8*>(cursor), src + (cursor - copy.address),
                                            begin - cursor);
                    Vulkan::SrtNoteReadback(cursor, begin - cursor);
                }
                cursor = std::max(cursor, hole_end);
                label_guard_hits.fetch_add(1, std::memory_order_relaxed);
            }
            if (cursor < end) {
                memory->TryWriteBacking(std::bit_cast<u8*>(cursor), src + (cursor - copy.address),
                                        end - cursor);
                Vulkan::SrtNoteReadback(cursor, end - cursor);
            }
        }
        const u64 hits = label_guard_hits.load(std::memory_order_relaxed);
        static u64 printed = 0;
        if (hits && (hits >> 6) != (printed >> 6)) {
            printed = hits;
            std::printf("uc3: async readback label guard: %llu labels kept\n",
                        static_cast<unsigned long long>(hits));
        }
    } else {
        for (const auto& copy : readback.copies) {
            auto* dst_addr = std::bit_cast<u8*>(copy.address);
            Gow3WriteLog::NoteDownload(copy.address, download.mapped + copy.staging_offset, copy.size);
            memory->TryWriteBacking(dst_addr, download.mapped + copy.staging_offset, copy.size);
            Vulkan::SrtNoteReadback(copy.address, copy.size);
        }
    }
    {
        std::scoped_lock lk{async_mutex};
        if (!readback.dirty) {
            for (const auto& window : readback.windows) {
                memory_tracker->UnmarkRegionAsGpuModified(window.start, window.size, false);
            }
            if (is_write) {
                memory_tracker->MarkRegionAsCpuModified(device_addr, size);
            }
        } else {
            Gow3Stats::uc3_rb_async_dirty.fetch_add(1, std::memory_order_relaxed);
        }
        std::erase_if(async_pending, [&](const auto& entry) { return entry.get() == &readback; });
        if (readback.fast_slot != ~0u) {
            std::scoped_lock fast_lk{fast_mutex};
            fast_free_slots.push_back(readback.fast_slot);
        } else {
            async_free.push_back(download);
            async_free_count.fetch_add(1, std::memory_order_release);
        }
        async_count.fetch_sub(1, std::memory_order_release);
    }
    readback.done.store(true, std::memory_order_release);
    readback.done.notify_all();
    Gow3Stats::uc3_rb_async_ns.fetch_add(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                             start)
            .count(),
        std::memory_order_relaxed);
}

void BufferCache::WaitAsyncReadbacks(VAddr start, VAddr end) {
    if (async_count.load(std::memory_order_acquire) == 0 &&
        async_free_count.load(std::memory_order_acquire) == 0) {
        return;
    }
    for (;;) {
        std::vector<Vulkan::StagingBufferRef> to_free;
        std::shared_ptr<AsyncReadback> overlap;
        {
            std::scoped_lock lk{async_mutex};
            to_free.swap(async_free);
            for (const auto& entry : async_pending) {
                const bool hit =
                    end == 0 || std::ranges::any_of(entry->windows, [&](const DownloadWindow& w) {
                        return w.start < end && start < w.start + w.size;
                    });
                if (hit) {
                    overlap = entry;
                    break;
                }
            }
        }
        for (const auto& ref : to_free) {
            staging_pool.FreeDeferred(ref);
        }
        async_free_count.fetch_sub(static_cast<u32>(to_free.size()), std::memory_order_release);
        if (!overlap) {
            return;
        }
        Gow3Stats::uc3_rb_async_overlap.fetch_add(1, std::memory_order_relaxed);
        overlap->done.wait(false, std::memory_order_acquire);
    }
}

void BufferCache::QueueLeafDownload(const Buffer* buffer, u64 offset, VAddr address, u32 size) {
    if (size > LeafSlotSize) {
        return;
    }
    u32 slot;
    {
        std::scoped_lock lk{leaf_mutex};
        for (const auto& leaf : leaf_pending) {
            if (leaf.address == address && !leaf.dirty) {
                return; // already on its way
            }
        }
        if (!leaf_staging.buffer) {
            leaf_staging = staging_pool.Request(u64{LeafSlotSize} * LeafSlots,
                                                VideoCore::MemoryType::HostCached, 0, true);
            leaf_free_slots.reserve(LeafSlots);
            for (u32 i = LeafSlots; i-- > 0;) {
                leaf_free_slots.push_back(i);
            }
        }
        if (leaf_free_slots.empty()) {
            Gow3Stats::uc3_leaf_dl_full.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        slot = leaf_free_slots.back();
        leaf_free_slots.pop_back();
        leaf_pending.push_back({address, size, slot, scheduler.CurrentTick(), false});
        leaf_pending_count.fetch_add(1, std::memory_order_release);
    }
    const vk::BufferCopy copy{offset, leaf_staging.offset + u64{slot} * LeafSlotSize, size};
    runtime.CopyBuffer(buffer, leaf_staging.buffer, {&copy, 1});
    Gow3Stats::uc3_leaf_dl_queued.fetch_add(1, std::memory_order_relaxed);
}

void BufferCache::CompleteLeafDownloads() {
    if (leaf_pending_count.load(std::memory_order_acquire) == 0) {
        return;
    }
    auto* semaphore = scheduler.GetWorkSemaphore();
    semaphore->Refresh();
    std::scoped_lock lk{leaf_mutex};
    const auto ranges_lock = RangesLock();
    bool invalidated = false;
    std::erase_if(leaf_pending, [&](const LeafDownload& leaf) {
        if (!semaphore->IsFree(leaf.tick)) {
            return false;
        }
        leaf_free_slots.push_back(leaf.slot);
        leaf_pending_count.fetch_sub(1, std::memory_order_release);
        if (leaf.dirty) {
            Gow3Stats::uc3_leaf_dl_dirty.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        if (!invalidated) {
            invalidated = true;
            leaf_staging.Invalidate();
        }
        const u8* data = leaf_staging.mapped + u64{leaf.slot} * LeafSlotSize;
        auto* dst = std::bit_cast<u8*>(leaf.address);
        Gow3WriteLog::NoteDownload(leaf.address, data, leaf.size);
        memory->TryWriteBacking(dst, data, leaf.size);
        Vulkan::SrtNoteReadback(leaf.address, leaf.size);
        gpu_modified_ranges.Subtract(leaf.address, leaf.size);
        // The page keeps its GPU state while other GPU bytes on it are still not downloaded.
        const VAddr page = leaf.address & ~VAddr{0xfff};
        if (!gpu_modified_ranges.Intersects(page, 0x1000)) {
            memory_tracker->UnmarkRegionAsGpuModified(page, 0x1000, false);
        }
        Gow3Stats::uc3_leaf_dl_landed.fetch_add(1, std::memory_order_relaxed);
        return true;
    });
}

bool BufferCache::IsGpuWritten(VAddr addr, u64 size) {
    {
        const auto ranges_lock = RangesLock();
        if (gpu_modified_ranges.Intersects(addr, size)) {
            return true;
        }
    }
    if (async_count.load(std::memory_order_acquire) == 0) {
        return false;
    }
    std::scoped_lock lk{async_mutex};
    for (const auto& entry : async_pending) {
        for (const auto& w : entry->windows) {
            if (w.start < addr + size && addr < w.start + w.size) {
                return true;
            }
        }
    }
    return false;
}

const char* BufferCache::DescribeGpuWritten(VAddr addr, u64 size) {
    const auto ranges_lock = RangesLock();
    static thread_local char text[256];
    if (gpu_modified_ranges.Intersects(addr, size)) {
        // The GPU-written pieces of the whole page, to see what the load shares it with.
        int length = std::snprintf(text, sizeof(text), "gpu_modified_ranges in page:");
        gpu_modified_ranges.ForEachInRange(addr & ~0xfffULL, 0x1000, [&](VAddr start, VAddr end) {
            if (length < 200) {
                length += std::snprintf(text + length, sizeof(text) - length, " [%#llx,%#llx)",
                                        (unsigned long long)start, (unsigned long long)end);
            }
        });
        return text;
    }
    return async_count.load(std::memory_order_acquire) ? "async readback (or image)"
                                                       : "image readback";
}

void BufferCache::NoteAsyncWrite(VAddr device_addr, u64 size) {
    if (async_count.load(std::memory_order_acquire) == 0) {
        return;
    }
    std::scoped_lock lk{async_mutex};
    for (const auto& entry : async_pending) {
        for (const auto& w : entry->windows) {
            if (w.start < device_addr + size && device_addr < w.start + w.size) {
                entry->dirty = true;
            }
        }
    }
}

void BufferCache::DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size) {
    const auto ranges_lock = RangesLock();
    boost::container::small_vector<vk::BufferCopy, 1> copies;
    u64 total_size_bytes = 0;
    const VAddr arena_base = arena->cpu_addr;
    memory_tracker->ForEachDownloadRange<false>(device_addr, size, [&](u64 address, u64 size) {
        const auto add_download = [&](VAddr start, VAddr end) {
            const u64 new_offset = start - arena_base;
            const u64 new_size = end - start;
            copies.push_back(vk::BufferCopy{
                .srcOffset = new_offset,
                .dstOffset = total_size_bytes,
                .size = new_size,
            });
            // Align up to avoid cache conflicts
            constexpr u64 align = 64ULL;
            constexpr u64 mask = ~(align - 1ULL);
            total_size_bytes += (new_size + align - 1) & mask;
        };
        gpu_modified_ranges.ForEachInRange(address, size, add_download);
        gpu_modified_ranges.Subtract(address, size);
    });
    if (total_size_bytes == 0) {
        return;
    }
    const auto download = staging_pool.Request(total_size_bytes, VideoCore::MemoryType::HostCached);
    for (auto& copy : copies) {
        copy.dstOffset += download.offset;
    }
    const SideCopies side{arena, copies};
    if (!SideDownload(std::span{&side, 1}, download.buffer, LastWriteTick(device_addr, size))) {
        runtime.CopyBuffer(arena, download.buffer, copies);
        scheduler.Finish();
        if (ReadbackFast()) {
            Gow3Stats::uc3_rb_full.fetch_add(1, std::memory_order_relaxed);
        }
    }

    download.buffer->Invalidate(download.offset, download.size);
    for (const auto& copy : copies) {
        auto* dst_addr = std::bit_cast<u8*>(arena_base + copy.srcOffset);
        Gow3WriteLog::NoteDownload(reinterpret_cast<u64>(dst_addr),
                                   download.mapped + (copy.dstOffset - download.offset), copy.size);
        memory->TryWriteBacking(dst_addr, download.mapped + (copy.dstOffset - download.offset),
                                copy.size);
        Vulkan::SrtNoteReadback(reinterpret_cast<VAddr>(dst_addr), copy.size);
    }
    memory_tracker->UnmarkRegionAsGpuModified(device_addr, size, false);
}

void BufferCache::DownloadWindows(std::span<const DownloadWindow> windows) {
    const auto ranges_lock = RangesLock();
    struct WindowCopies {
        const DownloadWindow* window;
        boost::container::small_vector<vk::BufferCopy, 4> copies;
    };
    boost::container::small_vector<WindowCopies, 17> work;
    u64 total_size_bytes = 0;
    for (const auto& window : windows) {
        WindowCopies entry{&window};
        const VAddr arena_base = window.arena->cpu_addr;
        memory_tracker->ForEachDownloadRange<false>(
            window.start, window.size, [&](u64 address, u64 size) {
                gpu_modified_ranges.ForEachInRange(address, size, [&](VAddr start, VAddr end) {
                    entry.copies.push_back(vk::BufferCopy{
                        .srcOffset = start - arena_base,
                        .dstOffset = total_size_bytes,
                        .size = end - start,
                    });
                    constexpr u64 align = 64ULL;
                    total_size_bytes += (end - start + align - 1) & ~(align - 1);
                });
                gpu_modified_ranges.Subtract(address, size);
            });
        if (!entry.copies.empty()) {
            work.push_back(std::move(entry));
        }
    }
    if (total_size_bytes == 0) {
        return;
    }
    const auto download = staging_pool.Request(total_size_bytes, VideoCore::MemoryType::HostCached);
    u64 write_tick = 0;
    boost::container::small_vector<SideCopies, 17> side;
    for (auto& entry : work) {
        for (auto& copy : entry.copies) {
            copy.dstOffset += download.offset;
        }
        write_tick = std::max(write_tick, LastWriteTick(entry.window->start, entry.window->size));
        side.push_back({entry.window->arena, entry.copies});
    }
    if (!SideDownload(side, download.buffer, write_tick)) {
        for (auto& entry : work) {
            runtime.CopyBuffer(entry.window->arena, download.buffer, entry.copies);
        }
        scheduler.Finish();
        if (ReadbackFast()) {
            Gow3Stats::uc3_rb_full.fetch_add(1, std::memory_order_relaxed);
        }
    }

    download.buffer->Invalidate(download.offset, download.size);
    for (const auto& entry : work) {
        const VAddr arena_base = entry.window->arena->cpu_addr;
        for (const auto& copy : entry.copies) {
            auto* dst_addr = std::bit_cast<u8*>(arena_base + copy.srcOffset);
            Gow3WriteLog::NoteDownload(reinterpret_cast<u64>(dst_addr),
                                       download.mapped + (copy.dstOffset - download.offset), copy.size);
            memory->TryWriteBacking(dst_addr, download.mapped + (copy.dstOffset - download.offset),
                                    copy.size);
            Vulkan::SrtNoteReadback(reinterpret_cast<VAddr>(dst_addr), copy.size);
        }
    }
    for (const auto& window : windows) {
        memory_tracker->UnmarkRegionAsGpuModified(window.start, window.size, false);
    }
}

void BufferCache::DownloadAllGpuModified() {
    const auto ranges_lock = RangesLock();
    struct ArenaCopies {
        const Buffer* arena;
        boost::container::small_vector<vk::BufferCopy, 16> copies;
    };
    boost::container::small_vector<ArenaCopies, 8> work;
    u64 total_size_bytes = 0;
    for (const Buffer& arena : arenas) {
        ArenaCopies entry{&arena};
        const VAddr arena_base = arena.cpu_addr;
        memory_tracker->ForEachDownloadRange<false>(
            arena_base, arena.size_bytes, [&](u64 address, u64 size) {
                gpu_modified_ranges.ForEachInRange(address, size, [&](VAddr start, VAddr end) {
                    entry.copies.push_back(vk::BufferCopy{
                        .srcOffset = start - arena_base,
                        .dstOffset = total_size_bytes,
                        .size = end - start,
                    });
                    constexpr u64 align = 64ULL;
                    total_size_bytes += (end - start + align - 1) & ~(align - 1);
                });
                gpu_modified_ranges.Subtract(address, size);
            });
        if (!entry.copies.empty()) {
            work.push_back(std::move(entry));
        }
    }
    if (total_size_bytes == 0) {
        return;
    }
    Gow3Stats::uc3_readback_all.fetch_add(1, std::memory_order_relaxed);
    Gow3Stats::uc3_readback_all_bytes.fetch_add(total_size_bytes, std::memory_order_relaxed);
    const auto download = staging_pool.Request(total_size_bytes, VideoCore::MemoryType::HostCached);
    for (auto& entry : work) {
        for (auto& copy : entry.copies) {
            copy.dstOffset += download.offset;
        }
        runtime.CopyBuffer(entry.arena, download.buffer, entry.copies);
    }
    scheduler.Finish();

    download.buffer->Invalidate(download.offset, download.size);
    for (const auto& entry : work) {
        const VAddr arena_base = entry.arena->cpu_addr;
        for (const auto& copy : entry.copies) {
            auto* dst_addr = std::bit_cast<u8*>(arena_base + copy.srcOffset);
            Gow3WriteLog::NoteDownload(reinterpret_cast<u64>(dst_addr),
                                       download.mapped + (copy.dstOffset - download.offset), copy.size);
            memory->TryWriteBacking(dst_addr, download.mapped + (copy.dstOffset - download.offset),
                                    copy.size);
            Vulkan::SrtNoteReadback(reinterpret_cast<VAddr>(dst_addr), copy.size);
        }
        memory_tracker->UnmarkRegionAsGpuModified(entry.arena->cpu_addr, entry.arena->size_bytes,
                                                  false);
    }
}

namespace {
// gow3: GOW3_BUFFER_STATS=1 — how buffer bindings reach the GPU, by guest region (256 MiB),
// printed every 5 s: small read-only copies into the stream buffer, arena bindings, and the
// bytes those re-upload after CPU writes. Input for engine-level short paths.
struct BufferStats {
    struct Region {
        u64 stream_count{}, stream_bytes{}, arena_count{}, arena_bytes{}, upload_bytes{};
    };
    std::map<u64, Region> regions;
    /// Hot window: frame of the last binding per 64 KiB block, and the distribution of the
    /// frames between uses (1, 2, 3, 4, 5-8, 9-16, 17+): the ring's reuse distance.
    std::unordered_map<u64, u64> last_use;
    std::array<u64, 7> reuse{};
    std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
    u64 calls = 0;
};
/// 256 MiB regions; 1 MiB inside the hot 0x104xxxxxxx window (the engine's frame data).
BufferStats& Stats();
void NoteHotUse(VAddr address, u32 size) {
    if ((address >> 28) != 0x104) {
        return;
    }
    auto& st = Stats();
    const u64 frame = Gow3Stats::gpu_frames.load(std::memory_order_relaxed);
    for (u64 block = address >> 16; block <= (address + size - 1) >> 16; ++block) {
        auto [it, inserted] = st.last_use.try_emplace(block, frame);
        if (!inserted && it->second != frame) {
            const u64 d = frame - it->second;
            ++st.reuse[d <= 4 ? d - 1 : d <= 8 ? 4 : d <= 16 ? 5 : 6];
            it->second = frame;
        }
    }
}
u64 RegionKey(VAddr address) {
    return (address >> 28) == 0x104 ? (address >> 20) | (1ull << 40) : address >> 28;
}
bool BufferStatsEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("GOW3_BUFFER_STATS");
        return value && value[0] == '1';
    }();
    return enabled;
}
BufferStats& Stats() {
    static BufferStats stats;
    return stats;
}
void PrintBufferStats() {
    auto& st = Stats();
    if ((++st.calls & 4095) != 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - st.window).count();
    if (seconds < 5.0) {
        return;
    }
    std::printf("Buffer stats (%.1f s, per second): region, stream copies/bytes, arena bindings, "
                "re-uploaded bytes\n", seconds);
    std::printf("  hot window reuse distance in frames (1,2,3,4,5-8,9-16,17+): %llu %llu %llu %llu "
                "%llu %llu %llu\n",
                (unsigned long long)st.reuse[0], (unsigned long long)st.reuse[1],
                (unsigned long long)st.reuse[2], (unsigned long long)st.reuse[3],
                (unsigned long long)st.reuse[4], (unsigned long long)st.reuse[5],
                (unsigned long long)st.reuse[6]);
    st.reuse = {};
    for (const auto& [region, r] : st.regions) {
        const u64 base = (region >> 40) ? (region & ((1ull << 40) - 1)) << 20 : region << 28;
        if ((region >> 40) && r.stream_bytes + r.upload_bytes < 1e6 * seconds) {
            continue; // quiet MiB of the hot window
        }
        std::printf("  %#012llx: %8.0f copies %8.2f MB, %8.0f arena %8.2f MB bound, %8.2f MB uploads\n",
                    static_cast<unsigned long long>(base), r.stream_count / seconds,
                    r.stream_bytes / seconds / 1e6, r.arena_count / seconds,
                    r.arena_bytes / seconds / 1e6,
                    r.upload_bytes / seconds / 1e6);
    }
    st.regions.clear();
    st.window = now;
}
} // namespace

std::pair<const Buffer*, u64> BufferCache::ObtainBuffer(VAddr device_addr, u32 size,
                                                        bool is_written, bool is_texel_buffer) {
    texture_cache.ResolveReadbacks(device_addr, size, true, ReadbackSource::BufferObtain);
    const bool stats = BufferStatsEnabled();
    if (stats) {
        PrintBufferStats();
        NoteHotUse(device_addr, size);
    }
    // For read-only buffers use device local stream buffer to reduce renderpass breaks.
    if (!is_written && size <= STREAM_THRESHOLD && !IsRegionGpuModified(device_addr, size)) {
        if (stats) {
            auto& region = Stats().regions[RegionKey(device_addr)];
            ++region.stream_count;
            region.stream_bytes += size;
        }
        // gow3: the guest data is copied on a copy thread, started now; submission and
        // guest-visible fences wait for it (Scheduler::WaitHostCopies).
        if (!stream_buffer.mapped_data.empty() &&
            !Gow3Toggle::Disabled(Gow3Toggle::DeferredStreamCopies)) {
            if (const auto offset = stream_buffer.Reserve(size, instance.UniformMinAlignment())) {
                SmallGuestCopy({
                    .run = &RunGuestCopy,
                    .context = this,
                    .source = device_addr,
                    .destination = reinterpret_cast<u64>(stream_buffer.mapped_data.data() + *offset),
                    .size = size,
                    .extra = reinterpret_cast<u64>(static_cast<Buffer*>(&stream_buffer)),
                });
                return {&stream_buffer, *offset};
            }
        }
        const auto [data, offset] = stream_buffer.Map(size, instance.UniformMinAlignment());
        memory->CopySparseMemory(device_addr, data, size);
        stream_buffer.Commit();
        return {&stream_buffer, offset};
    }
    const u64 first_block = device_addr >> block_shift;
    const u64 last_block = (device_addr + size - 1) >> block_shift;
    const auto* arena = GetArena(first_block, last_block);
    EnsureResident(arena, first_block, last_block);
    const u64 uploaded_before = Gow3Stats::buffer_upload_bytes.load(std::memory_order_relaxed);
    if (is_written) {
        NoteAsyncWrite(device_addr, size);
        if (leaf_pending_count.load(std::memory_order_acquire)) {
            std::scoped_lock lk{leaf_mutex};
            for (auto& leaf : leaf_pending) {
                if (leaf.address < device_addr + size && device_addr < leaf.address + leaf.size) {
                    leaf.dirty = true;
                }
            }
        }
    }
    SynchronizeMemory(arena, device_addr, size, is_written, is_texel_buffer);
    if (stats) {
        auto& region = Stats().regions[RegionKey(device_addr)];
        ++region.arena_count;
        region.arena_bytes += size;
        region.upload_bytes +=
            Gow3Stats::buffer_upload_bytes.load(std::memory_order_relaxed) - uploaded_before;
    }
    if (is_written) {
        // GOW3_READBACK_DONE_FAST: the range and its write tick change together.
        const auto ranges_lock = RangesLock();
        gpu_modified_ranges.Add(device_addr, size);
        NoteGpuWrite(device_addr, size);
        // uc3 R.10 A1: stamped where the write binds
        Vulkan::SrtNoteLanding(device_addr, size, Vulkan::SrtOriginBinding);
    }
    return {arena, arena->Offset(device_addr)};
}

std::pair<const Buffer*, u64> BufferCache::ObtainBufferForImage(VAddr device_addr, u32 size) {
    texture_cache.ResolveReadbacks(device_addr, size, true, ReadbackSource::BufferForImage);
    if (IsRegionGpuModified(device_addr, size)) {
        return ObtainBuffer(device_addr, size, false);
    }
    const auto staging = staging_pool.Request(size, VideoCore::MemoryType::HostUncached,
                                              instance.StorageMinAlignment());
    if (!Gow3Toggle::Disabled(Gow3Toggle::DeferredUploads)) {
        // gow3: texture data is copied on the copy threads (streaming: 100+ MB per frame);
        // the upload reads the staging only after submission, which waits for the copies.
        constexpr u64 Chunk = 1_MB;
        for (u64 offset = 0; offset < staging.size; offset += Chunk) {
            const Gow3Copy::Item item{
                .run = &RunGuestCopy,
                .context = this,
                .source = device_addr + offset,
                .destination = reinterpret_cast<u64>(staging.mapped + offset),
                .size = std::min(Chunk, staging.size - offset),
                .extra = reinterpret_cast<u64>(staging.buffer),
            };
            if (staging.size < Chunk) {
                SmallGuestCopy(item);
            } else {
                NotePendingHostRead(item.source, item.size, +1);
                Gow3Copy::Async([item] { item.run(item); });
            }
        }
        return {staging.buffer, staging.offset};
    }
    memory->CopySparseMemory(device_addr, staging.mapped, staging.size);
    staging.Flush();
    return {staging.buffer, staging.offset};
}

bool BufferCache::IsRegionCpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionCpuModified(addr, size);
}

bool BufferCache::IsRegionGpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionGpuModified(addr, size);
}

void BufferCache::SynchronizeDmaBuffers() {
    fault_process_pending = true;
    for (const auto& range : resident_ranges) {
        const u64 page = range.start >> (arena_page_bits - block_shift);
        const VAddr device_addr = range.start << block_shift;
        const u64 size = (range.end - range.start) << block_shift;
        SynchronizeMemory(address_space[page], device_addr, size, false, false);
    }
}

const Buffer* BufferCache::GetArena(u64 first_block, u64 last_block) {
    const u64 first_page = first_block >> blocks_per_arena_page_shift;
    const u64 last_page = last_block >> blocks_per_arena_page_shift;
    ASSERT_MSG(last_page - first_page <= 1,
               "Buffer request cannot span more than two VA arena pages");

    const auto* first_arena = address_space[first_page];
    const auto* last_arena = address_space[last_page];
    if (first_arena == last_arena) {
        if (!first_arena) {
            const u64 base_block = Common::AlignDownPow2<u64>(first_block, blocks_per_arena_page);
            const u64 num_pages = last_page - first_page + 1;
            const auto* new_arena =
                &arenas.emplace_back(instance, base_block << block_shift,
                                     num_pages << arena_page_bits, MemoryType::Sparse);
            address_space[first_page] = new_arena;
            address_space[last_page] = new_arena;
        }
        return address_space[first_page];
    }

    LOG_WARNING(Render, "Migrating arena");

    const u64 first_addr = first_arena ? first_arena->cpu_addr : (first_page << arena_page_bits);
    const u64 first_size = first_arena ? first_arena->size_bytes : arena_page_size;
    const u64 last_size = last_arena ? last_arena->size_bytes : arena_page_size;

    const u64 base_block = first_addr >> block_shift;
    const u64 total_size = first_size + last_size;
    const u64 end_block = (first_addr + total_size) >> block_shift;
    auto* new_arena = &arenas.emplace_back(instance, first_addr, total_size, MemoryType::Sparse);
    auto* bind = BindsForArena(new_arena);
    resident_ranges.ForEachInRange(base_block, end_block, [&](const Backing& backing) {
        const u64 start = std::max(base_block, backing.start);
        const u64 end = std::min(end_block, backing.end);
        bind->binds.push_back(vk::SparseMemoryBind{
            .resourceOffset = (start - base_block) << block_shift,
            .size = (end - start) << block_shift,
            .memory = backing.memory,
            .memoryOffset = (backing.offset + start - backing.start) << block_shift,
        });
    });

    u64 base_page = first_addr >> arena_page_bits;
    for (u32 page = 0; page < (first_size >> arena_page_bits); ++page) {
        address_space[base_page + page] = new_arena;
    }
    base_page = last_page;
    for (u32 page = 0; page < (last_size >> arena_page_bits); ++page) {
        address_space[base_page + page] = new_arena;
    }
    return new_arena;
}

void BufferCache::EnsureResident(const Buffer* arena, u64 first_block, u64 last_block) {
    u32 resident_blocks{};
    IntervalList bind_ranges;
    resident_ranges.ForEachGap(first_block, last_block + 1, [&](u64 start, u64 end) {
        resident_blocks += end - start;
        bind_ranges.Add({start, end});
    });

    if (bind_ranges.Empty()) {
        return;
    }
    Gow3Stats::Timer timer{Gow3Stats::t_resident};

    const vk::MemoryAllocateFlagsInfo alloc_flags = {
        .flags = vk::MemoryAllocateFlagBits::eDeviceAddress,
    };
    const vk::MemoryAllocateInfo alloc_info = {
        .pNext = &alloc_flags,
        .allocationSize = resident_blocks << block_shift,
        .memoryTypeIndex = arena_memory_type_index,
    };
    const auto device_memory = Vulkan::Check(instance.GetDevice().allocateMemory(alloc_info));

    boost::container::small_vector<vk::BufferCopy, 8> copies;
    const auto staging =
        staging_pool.Request(resident_blocks * sizeof(vk::DeviceAddress), MemoryType::HostUncached);

    u64 memory_offset{};
    ArenaBinds* binds = BindsForArena(arena);
    auto* bda_addrs = reinterpret_cast<vk::DeviceAddress*>(staging.mapped);
    u64 offset = staging.offset;
    for (const auto& range : bind_ranges) {
        Backing backing;
        backing.start = range.start;
        backing.end = range.end;
        backing.memory = device_memory;
        backing.offset = memory_offset >> block_shift;
        resident_ranges.Add(backing);

        LOG_INFO(Render, "Making range start={}, end={} resident", backing.start, backing.end);

        const auto& bind = binds->binds.emplace_back(vk::SparseMemoryBind{
            .resourceOffset = (range.start << block_shift) - arena->cpu_addr,
            .size = (range.end - range.start) << block_shift,
            .memory = device_memory,
            .memoryOffset = memory_offset,
        });
        memory_offset += bind.size;

        for (u32 block = 0; block < bind.size; block += block_size) {
            *(bda_addrs++) = arena->BufferDeviceAddress() + bind.resourceOffset + block;
        }
        const u64 copy_size = (backing.end - backing.start) * sizeof(vk::DeviceAddress);
        copies.emplace_back(offset, backing.start * sizeof(vk::DeviceAddress), copy_size);
        offset += copy_size;
    }

    staging.Flush();
    runtime.CopyBuffer(staging.buffer, bda_pagetable_buffer.get(), copies);
}

bool BufferCache::SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size,
                                    bool is_written, bool is_texel_buffer) {
    boost::container::small_vector<vk::BufferCopy, 4> copies;
    size_t total_size_bytes{};
    memory_tracker->ForEachUploadRange(device_addr, size, is_written, [&](u64 addr, u64 size) {
        copies.emplace_back(total_size_bytes, addr, size);
        total_size_bytes += size;
        NoteArenaWrite(addr, size);
    });
    if (const Buffer* src_buffer = UploadCopies(arena, copies, total_size_bytes)) {
        runtime.CopyBuffer(src_buffer, arena, copies);
    }
    if (is_texel_buffer && !is_written) {
        return SynchronizeMemoryFromImage(arena, device_addr, size);
    }
    return false;
}

const Buffer* BufferCache::UploadCopies(const Buffer* arena, std::span<vk::BufferCopy> copies,
                                        size_t total_size_bytes) {
    if (copies.empty()) {
        return nullptr;
    }
    // Resolve before launching host copies: a copy-thread fault could deadlock submission.
    for (const auto& copy : copies) {
        texture_cache.ResolveReadbacks(copy.dstOffset, copy.size, true, ReadbackSource::BufferUpload);
    }
    Gow3Stats::buffer_upload_bytes.fetch_add(total_size_bytes, std::memory_order_relaxed);
    const auto staging = staging_pool.Request(total_size_bytes, MemoryType::HostUncached);
    // gow3: the guest memory is copied into staging on the copy threads, started now in
    // groups of about 1 MiB; submission and guest-visible fences wait for them
    // (Scheduler::WaitHostCopies).
    if (!Gow3Toggle::Disabled(Gow3Toggle::DeferredUploads) && total_size_bytes < 1_MB) {
        // Small uploads join the calling thread's batch.
        for (auto& copy : copies) {
            SmallGuestCopy({
                .run = &RunGuestCopy,
                .context = this,
                .source = copy.dstOffset,
                .destination = reinterpret_cast<u64>(staging.mapped + copy.srcOffset),
                .size = copy.size,
                .extra = reinterpret_cast<u64>(staging.buffer),
            });
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
        }
        return staging.buffer;
    }
    if (!Gow3Toggle::Disabled(Gow3Toggle::DeferredUploads)) {
        struct HostCopy {
            VAddr source;
            u8* destination;
            u64 size;
        };
        using Group = boost::container::small_vector<HostCopy, 8>;
        auto group = std::make_shared<Group>();
        u64 group_bytes = 0;
        const auto launch = [&] {
            Gow3Copy::Async([group, memory = memory, staging] {
                for (const auto& copy : *group) {
                    memory->CopySparseMemory(copy.source, copy.destination, copy.size);
                }
                staging.Flush();
                for (const auto& copy : *group) {
                    NotePendingHostRead(copy.source, copy.size, -1);
                }
            });
        };
        for (auto& copy : copies) {
            NotePendingHostRead(copy.dstOffset, copy.size, +1);
            group->push_back({copy.dstOffset, staging.mapped + copy.srcOffset, copy.size});
            group_bytes += copy.size;
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
            if (group_bytes >= 1_MB) {
                launch();
                group = std::make_shared<Group>();
                group_bytes = 0;
            }
        }
        if (!group->empty()) {
            launch();
        }
        return staging.buffer;
    }
    const auto copy = [&](std::size_t i) {
        memory->CopySparseMemory(copies[i].dstOffset, staging.mapped + copies[i].srcOffset,
                                 copies[i].size);
    };
    if (total_size_bytes >= 2_MB && copies.size() > 1) {
        Gow3Copy::ParallelFor(copies.size(), copy);
    } else {
        for (std::size_t i = 0; i < copies.size(); ++i) {
            copy(i);
        }
    }
    for (auto& copy : copies) {
        copy.srcOffset += staging.offset;
        copy.dstOffset -= arena->cpu_addr;
    }
    staging.Flush();
    return staging.buffer;
}

bool BufferCache::SyncImageToBuffer(VAddr addr, u32 size) {
    if (!texture_cache.FindImageFromRange(addr, size)) {
        return false;
    }
    const auto [arena, offset] = ObtainBuffer(addr, size, true);
    return SynchronizeMemoryFromImage(arena, addr, size);
}

bool BufferCache::SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size) {
    if (const auto meta = texture_cache.GetMetaInfo(device_addr)) {
        if (meta->type == TextureCache::MetaType::HTile) {
            static constexpr u32 ZmaskUncompressed = 0xf;
            runtime.FillBuffer(arena, arena->Offset(device_addr), size, ZmaskUncompressed);
            NoteArenaWrite(device_addr, size);
            return true;
        } else if (meta->type == TextureCache::MetaType::CMask) {
            const auto expanded_value = CmaskColorExpandedValue(meta->num_samples);
            if (!expanded_value) {
                LOG_WARNING(Render_Vulkan,
                            "Unsupported CMask sample count: address={:#x}, size={:#x}, "
                            "samples={}, owner={}:{}",
                            device_addr, size, meta->num_samples, meta->owner_id.index,
                            meta->owner_uid);
                return false;
            }
            runtime.FillBuffer(arena, arena->Offset(device_addr), size, *expanded_value);
            NoteArenaWrite(device_addr, size);
            return true;
        } else {
            LOG_WARNING(Render_Vulkan, "Unhandled metadata type {}",
                        magic_enum::enum_name(meta->type));
        }
    }
    // gow3: most texel buffers alias no image; remember misses until images change.
    const u64 generation = texture_cache.RegistryGeneration();
    auto& miss = image_miss_cache[((device_addr >> 6) ^ size * 0x9E3779B1u) % image_miss_cache.size()];
    if (miss.address == device_addr && miss.size == size && miss.generation == generation &&
        !Gow3Toggle::Disabled(Gow3Toggle::TextureBindingMemo)) {
        return false;
    }
    const ImageId image_id = texture_cache.FindImageFromRange(device_addr, size);
    if (!image_id) {
        miss = {device_addr, size, generation};
        return false;
    }
    // gow3: the lookups above only read what the texture binding helper leaves alone; the
    // copy below changes image state, so the helper finishes first.
    runtime.BeforeImageAccess();
    Image& image = texture_cache.GetImage(image_id);
    ASSERT_MSG(device_addr == image.info.guest_address,
               "Texel buffer aliases image subresources {:x} : {:x}", device_addr,
               image.info.guest_address);
    const u64 arena_offset = arena->Offset(device_addr);
    boost::container::small_vector<vk::BufferImageCopy, 8> buffer_copies;
    for (u32 mip = 0; mip < image.info.resources.levels; mip++) {
        const auto& mip_info = image.info.mips_layout[mip];
        const u32 width = std::max(image.info.size.width >> mip, 1u);
        const u32 height = std::max(image.info.size.height >> mip, 1u);
        const u32 depth = std::max(image.info.size.depth >> mip, 1u);
        if (arena_offset + mip_info.offset + mip_info.size > arena->size_bytes) {
            break;
        }
        buffer_copies.push_back(vk::BufferImageCopy{
            .bufferOffset = mip_info.offset,
            .bufferRowLength = mip_info.pitch,
            .bufferImageHeight = mip_info.height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, depth},
        });
    }
    if (buffer_copies.empty()) {
        return false;
    }
    auto& tile_manager = texture_cache.GetTileManager();
    tile_manager.TileImage(image, buffer_copies, arena, arena_offset);
    NoteArenaWrite(device_addr, size);
    return true;
}

void BufferCache::SubmitPendingArenaBinds(Vulkan::SubmitInfo& info) {
    if (pending_binds.empty()) {
        return;
    }

    std::vector<vk::SparseBufferMemoryBindInfo> buffer_binds;
    buffer_binds.reserve(pending_binds.size());

    for (const auto& binds : pending_binds) {
        buffer_binds.emplace_back(vk::SparseBufferMemoryBindInfo{
            .buffer = binds.arena->Handle(),
            .bindCount = static_cast<u32>(binds.binds.size()),
            .pBinds = binds.binds.data(),
        });
    }

    const u64 signal_tick = memory_semaphore.NextTick();
    const auto signal_sema = memory_semaphore.Handle();

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .signalSemaphoreValueCount = 1u,
        .pSignalSemaphoreValues = &signal_tick,
    };

    const vk::BindSparseInfo sparse_info = {
        .pNext = &timeline_si,
        .bufferBindCount = static_cast<u32>(buffer_binds.size()),
        .pBufferBinds = buffer_binds.data(),
        .signalSemaphoreCount = 1u,
        .pSignalSemaphores = &signal_sema,
    };

    info.AddWait(signal_sema, signal_tick);
    auto submit_result = instance.GetGraphicsQueue().bindSparse(sparse_info);
    ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");

    pending_binds.clear();
}

} // namespace VideoCore
