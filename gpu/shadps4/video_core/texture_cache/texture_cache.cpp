// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <mutex>
#include <unordered_set>
#include <cstdio>
#include <string>
#include <cstdlib>
#include <chrono>
#include <xxhash.h>
#include "video_core/amdgpu/liverpool.h"

#include "gow3_toggles.h"
#include "common/assert.h"
#include "common/debug.h"
#include "common/div_ceil.h"
#include "common/hash.h"
#include "common/scope_exit.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "gow3_write_log.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h" // uc3 R.10.17: SrtNoteReadback
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_wait_diagnostics.h"
#include "video_core/texture_cache/aliasing.h"
#include "video_core/texture_cache/host_compatibility.h"
#include "video_core/texture_cache/texture_cache.h"
#include "video_core/texture_cache/tile_manager.h"

namespace VideoCore {

// uc3: GOW3_WATCH_ADDR=<hex guest addr>: logs every image event touching that address (who
// writes the cave torch's 256x8 table).
static VAddr WatchAddr() {
    static const VAddr addr = [] {
        const char* env = std::getenv("GOW3_WATCH_ADDR");
        return env ? VAddr(std::strtoull(env, nullptr, 16)) : VAddr(0);
    }();
    return addr;
}

// uc3: GOW3_SKIP_LOG=1: color targets of draws skipped while their shaders compiled; an image
// sampled later without any successful write lost a one-shot draw (the cave torch table).
static std::mutex skipped_targets_mutex;
static std::unordered_set<VAddr> skipped_targets;
static std::atomic<bool> any_skipped_targets{false};

void NoteSkippedTarget(VAddr address) {
    std::scoped_lock lk{skipped_targets_mutex};
    if (skipped_targets.insert(address).second) {
        any_skipped_targets.store(true, std::memory_order_relaxed);
    }
}

static void CheckSkippedTarget(const Image& image, bool write) {
    if (!any_skipped_targets.load(std::memory_order_relaxed)) {
        return;
    }
    std::scoped_lock lk{skipped_targets_mutex};
    const auto it = skipped_targets.find(image.info.guest_address);
    if (it == skipped_targets.end()) {
        return;
    }
    if (!write && False(image.flags & ImageFlagBits::GpuModified)) {
        std::printf("ucndc: image %#llx %ux%u fmt %d sampled but never written (its draw was "
                    "skipped while shaders compiled)\n",
                    (unsigned long long)image.info.guest_address, image.info.size.width,
                    image.info.size.height, int(image.info.pixel_format));
        std::fflush(stdout);
    }
    skipped_targets.erase(it);
}

static void WatchLog(const char* what, const Image& image) {
    const VAddr addr = WatchAddr();
    if (!addr || !image.Overlaps(addr, 1)) {
        return;
    }
    static std::atomic<int> count{0};
    if (count.fetch_add(1) > 400) {
        return;
    }
    std::printf("ucndc: watch %s image %#llx size %#x %ux%ux%u fmt %d bits %u tiled %d flags %#x uid %llu\n",
                what, (unsigned long long)image.info.guest_address, image.info.guest_size,
                image.info.size.width, image.info.size.height, image.info.size.depth,
                int(image.info.pixel_format), image.info.num_bits, int(image.info.props.is_tiled),
                unsigned(image.flags), (unsigned long long)image.image_uid);
    std::fflush(stdout);
}
// uc3: GOW3_FLASH_TRACE time base: process start (the game window follows within seconds).
static const auto uc3_flash_clock = std::chrono::steady_clock::now();

// uc3: GOW3_EXPAND_FRESH=1 - when an image grows (more layers/levels), take the new image from
// guest memory and copy the old one into it only if the GPU wrote the old one (refreshed first).
// Before, the old image was always copied over the fresh upload, so a CPU-filled array whose old
// copy was stale showed dark for a moment (flickering wall/texture, SanitizeCopyLayers N -> N+1).
static bool ExpandFresh() {
    static const bool on = [] {
        const char* env = std::getenv("GOW3_EXPAND_FRESH");
        const bool v = env && env[0] == '1';
        if (v) {
            std::printf("Texture cache: grown images start from guest memory\n");
        }
        return v;
    }();
    return on;
}

// uc3: GOW3_EXPAND_TRACE=1 - diagnostic: one line per image expansion and per freed image with
// more than one layer (caller line, address, layers, flags), to find who shrinks an array back.
static bool ExpandTrace() {
    static const bool on = [] {
        const char* env = std::getenv("GOW3_EXPAND_TRACE");
        return env && env[0] == '1';
    }();
    return on;
}

// uc3: GOW3_DEPTH_KEEP_CLEAR=1 - a depth image recreated through overlap resolution (shadow map
// arrays grown one layer at a time) keeps the HTILE clear state of the image it replaces. Before,
// the new image registered its HTILE with clear mask 0, so a pending HTILE clear (ClearMeta) was
// lost and the next cascade was drawn over uncleared depth (flickering light/shadows).
static bool DepthKeepClear() {
    static const bool on = [] {
        const char* env = std::getenv("GOW3_DEPTH_KEEP_CLEAR");
        const bool v = env && env[0] == '1';
        if (v) {
            std::printf("Texture cache: recreated depth images keep their HTILE clear state\n");
        }
        return v;
    }();
    return on;
}

static double ExpandTraceTime() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - uc3_flash_clock)
        .count();
}

static bool ExpandTraceBudget() {
    static std::atomic<int> lines{0};
    return lines.fetch_add(1, std::memory_order_relaxed) < 50000;
}

static constexpr u32 MAX_IMAGES = std::numeric_limits<u16>::max();
static constexpr u32 MAX_IMAGE_VIEWS = std::numeric_limits<u16>::max();

TextureCache::TextureCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                           Vulkan::Runtime& runtime_, AmdGpu::Liverpool* liverpool_,
                           BufferCache& buffer_cache_, PageManager& tracker_)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_}, liverpool{liverpool_},
      buffer_cache{buffer_cache_}, tracker{tracker_}, slot_images{MAX_IMAGES},
      slot_image_views{MAX_IMAGE_VIEWS}, blit_helper{instance, scheduler},
      tile_manager{instance, scheduler, runtime, buffer_cache.GetStreamBuffer()},
      readback_linear_images{EmulatorSettings.IsReadbackLinearImagesEnabled()} {

    // uc3: the readback mode in the log, so a run's FPS is never attributed to the wrong mode.
    const u32 readbacks_mode = EmulatorSettings.GetReadbacksMode();
    std::printf("GPU: readbacks mode %u (%s); deferred image readback %s (64 copies, 64 MiB "
                "pending limit)%s\n",
                readbacks_mode,
                readbacks_mode == u32(GpuReadbacksMode::Precise)
                    ? "precise: GPU-written pages read-protected, GOW3_READBACKS=2"
                : readbacks_mode == u32(GpuReadbacksMode::Relaxed) ? "relaxed, GOW3_READBACKS=1"
                                                                   : "disabled",
                deferred_readbacks ? "enabled" : "disabled",
                stale_readbacks ? "; CPU reads use the previous frame's copy" : "");

    u32 max_samplers = instance.GetMaxSamplerAllocationCount();
    trigger_gc_samplers = max_samplers * 3 / 4;
    pressure_gc_samplers = max_samplers * 7 / 8;
    critical_gc_samplers = max_samplers * 15 / 16;

    // Set up garbage collection parameters.
    if (!instance.CanReportMemoryUsage()) {
        trigger_gc_memory = 0;
        pressure_gc_memory = DEFAULT_PRESSURE_GC_MEMORY;
        critical_gc_memory = DEFAULT_CRITICAL_GC_MEMORY;
        return;
    }

    const s64 device_local_memory = static_cast<s64>(instance.GetTotalMemoryBudget());
    const s64 min_spacing_expected = device_local_memory - 1_GB;
    const s64 min_spacing_critical = device_local_memory - 512_MB;
    const s64 mem_threshold = std::min<s64>(device_local_memory, TARGET_GC_THRESHOLD);
    const s64 min_vacancy_expected = (6 * mem_threshold) / 10;
    const s64 min_vacancy_critical = (2 * mem_threshold) / 10;
    const s64 min_pressure_floor =
        std::clamp<s64>(device_local_memory / 4, 256_MB, DEFAULT_PRESSURE_GC_MEMORY);
    const s64 min_critical_floor =
        std::clamp<s64>(device_local_memory / 2, 512_MB, DEFAULT_CRITICAL_GC_MEMORY);
    pressure_gc_memory = static_cast<u64>(
        std::max<s64>(std::min(device_local_memory - min_vacancy_expected, min_spacing_expected),
                      min_pressure_floor));
    critical_gc_memory = static_cast<u64>(
        std::max<s64>(std::min(device_local_memory - min_vacancy_critical, min_spacing_critical),
                      min_critical_floor));
    trigger_gc_memory = static_cast<u64>((device_local_memory - mem_threshold) / 2);
}

TextureCache::~TextureCache() {
    if (deferred_readbacks) {
        scheduler.Finish();
        std::scoped_lock lock{readback_mutex};
        RetireReadbacks();
    }
}

template <bool track>
void TextureCache::WatchReadback(VAddr address, u64 size) {
    constexpr auto op = track ? PageOp::Track : PageOp::Untrack;
    if (stale_readbacks) {
        tracker.UpdatePageWatchers(address, size, op);
    } else {
        tracker.UpdatePageWatchers(address, size, PageOp::None, op);
    }
}

void TextureCache::RetireReadbacks() {
    pending_readbacks.Retire([this](u64 tick) { return scheduler.IsFree(tick); },
        [this](auto& entry) {
            if (entry.valid) {
                entry.payload.Invalidate();
                Gow3WriteLog::ClobberCheck(entry.payload.mapped, entry.address,
                                           entry.payload.mapped, entry.size, "image deferred");
                Gow3WriteLog::NoteWriteSite(entry.address, entry.size, 1);
                Core::Memory::Instance()->TryWriteBacking(std::bit_cast<u8*>(entry.address),
                    entry.payload.mapped, entry.size);
                Vulkan::SrtNoteReadback(entry.address, entry.size);
                if (entry.watched) WatchReadback<false>(entry.address, entry.size);
                ++readbacks_completed;
            }
            runtime.GetStagingPool().FreeDeferred(entry.payload);
        });
    has_pending_readbacks.store(pending_readbacks.Bytes() != 0, std::memory_order_release);
}

bool TextureCache::ResolveReadbacks(VAddr address, u64 size, bool assume_locks,
                                    ReadbackSource source) {
    if (!deferred_readbacks || !has_pending_readbacks.load(std::memory_order_acquire)) return false;
    {
        std::scoped_lock lock{readback_mutex};
        // Stale mode, byte rule: God of War III writes every frame to the page that holds a
        // 1x1 exposure copy, but not to its bytes. That write need not wait for the GPU: the
        // copy only stops protecting its page and is still published when the GPU is done.
        if (stale_readbacks && source == ReadbackSource::CpuWrite) {
            readbacks_released += pending_readbacks.ReleaseOutside(address, size, 64_KB,
                [this](auto& entry) { WatchReadback<false>(entry.address, entry.size); });
        }
        if (!pending_readbacks.RequiredTick(address, size)) return false;
    }
    const auto resolve = [this, address, size, source] {
        Gow3Stats::Timer resolve_timer{Gow3Stats::resolve_ns};
        Gow3Stats::resolve_count.fetch_add(1, std::memory_order_relaxed);
        const bool diag = Vulkan::WaitDiagnostics::Enabled();
        std::scoped_lock lock{readback_mutex};
        bool waited = false;
        u64 wait_ns = 0;
        if (diag && (source == ReadbackSource::CpuRead || source == ReadbackSource::CpuWrite)) {
            // Which copied image the CPU touched while its copy was pending.
            for (auto& image : readback_images) {
                if (image.copies && address < image.address + image.Bytes() && image.address < address + size) {
                    ++image.cpu_accesses;
                }
            }
        }
        Gow3Stats::Timer readback_timer{Gow3Stats::t_readback_wait};
        if (const u64 tick = pending_readbacks.RequiredTick(address, size)) {
            ++readbacks_waited;
            waited = !scheduler.IsFree(tick);
            const auto start = diag ? std::chrono::steady_clock::now()
                                    : std::chrono::steady_clock::time_point{};
            scheduler.Wait(tick);
            if (diag) {
                wait_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now() - start).count();
            }
            RetireReadbacks();
        }
        if (diag) readback_sources.Record(source, waited, wait_ns, size);
    };
    if (assume_locks) resolve();
    else liverpool->SendCommand<true>(resolve);
    return true;
}

void TextureCache::CancelReadbacks(VAddr address, u64 size) {
    if (!deferred_readbacks) return;
    std::scoped_lock lock{readback_mutex};
    pending_readbacks.Cancel(address, size, [this](auto& entry) {
        Gow3WriteLog::ClobberDrop(entry.payload.mapped);
        if (entry.watched) WatchReadback<false>(entry.address, entry.size);
        ++readbacks_canceled;
    });
}

// uc3 (from SuperMedo's uncharted_pc 722df5f, GPL): U2's aiming autofocus renders the depth under
// the crosshair down to a 1x1 R32F image that the game reads on the CPU to set the depth of field.
// Render targets are not copied back to guest memory, so the CPU read a stale value: the focus sat
// at about 2 m and everything behind Drake was blurred while aiming. GOW3_TINY_READBACK=1 (off by
// default): images of at most 4x4 pixels (1x1 of any tiling, else linear) the GPU writes are copied
// back after each use, written to guest memory once the GPU has done it, without waiting.
bool TextureCache::TinyReadback(const Image& image) {
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_TINY_READBACK");
        return env && env[0] == '1';
    }();
    const auto& size = image.info.size;
    return enabled && image.info.guest_address != 0 && size.depth == 1 &&
           size.width * size.height <= 16 &&
           (!image.info.props.is_tiled || (size.width == 1 && size.height == 1));
}

// gow3: every copy is recorded first and the GPU is waited for once. A wait per image (as
// before) left the GPU mostly idle in God of War III's gameplay, at about 2 FPS.
void TextureCache::ProcessDownloadImages() {
    {
        std::unordered_set<ImageId> tiny;
        {
            std::unique_lock lock{download_images_mutex};
            tiny.swap(tiny_download_images);
        }
        for (const ImageId image_id : tiny) {
            DownloadImageMemory(image_id, false);
        }
    }
    if (deferred_readbacks) {
        // Upload workers must finish reading RAM before these pages become inaccessible.
        scheduler.WaitHostCopies();
        std::scoped_lock readback_lock{readback_mutex};
        RetireReadbacks();
        std::unique_lock lock{download_images_mutex};
        for (const ImageId image_id : download_images) {
            const auto& image = slot_images[image_id];
            if (False(image.flags & ImageFlagBits::GpuModified)) continue;
            const u64 size = u64{image.info.pitch} * image.info.size.height *
                image.info.size.depth * image.info.resources.layers * (image.info.num_bits / 8);
            if (!pending_readbacks.CanFit(size)) {
                ++readbacks_waited;
                scheduler.Finish();
                RetireReadbacks();
            }
            if (auto copy = RecordImageDownload(image_id)) {
                if (!pending_readbacks.CanFit(copy->size)) {
                    scheduler.Finish();
                    copy->staging.Invalidate();
                    Gow3WriteLog::NoteWriteSite(copy->guest_address, copy->size, 2);
                    Core::Memory::Instance()->TryWriteBacking(
                        std::bit_cast<u8*>(copy->guest_address), copy->staging.mapped, copy->size);
                    Vulkan::SrtNoteReadback(copy->guest_address, copy->size);
                    runtime.GetStagingPool().FreeDeferred(copy->staging);
                } else {
                    WatchReadback<true>(copy->guest_address, copy->size);
                    if (Vulkan::WaitDiagnostics::Enabled()) {
                        // Which images go back to RAM each frame (the report lists the busiest).
                        auto* slot = std::ranges::find(readback_images, image.info.guest_address,
                                                       &ReadbackImageStat::address);
                        if (slot == readback_images.end()) {
                            slot = std::ranges::find(readback_images, VAddr{0}, &ReadbackImageStat::address);
                        }
                        if (slot != readback_images.end()) {
                            *slot = {image.info.guest_address, image.info.size.width,
                                     image.info.size.height, image.info.num_bits, slot->copies + 1,
                                     slot->cpu_accesses, image.info.pixel_format};
                        }
                    }
                    Gow3WriteLog::ClobberSnap(copy->staging.mapped, copy->guest_address,
                                              copy->size);
                    pending_readbacks.Push(copy->guest_address, copy->size,
                        scheduler.CurrentTick(), copy->staging);
                    has_pending_readbacks.store(true, std::memory_order_release);
                    ++readbacks_queued;
                    readbacks_peak_bytes = std::max(readbacks_peak_bytes, pending_readbacks.Bytes());
                }
            }
        }
        download_images.clear();
        if (Vulkan::WaitDiagnostics::Enabled()) {
            const auto now = std::chrono::steady_clock::now();
            if (now - readback_report >= std::chrono::seconds(5)) {
                std::printf("Readback profile: queued %llu, completed %llu, demand/budget waits %llu, canceled %llu, pending %.2f MiB, peak %.2f MiB\n",
                    static_cast<unsigned long long>(readbacks_queued),
                    static_cast<unsigned long long>(readbacks_completed),
                    static_cast<unsigned long long>(readbacks_waited),
                    static_cast<unsigned long long>(readbacks_canceled),
                    pending_readbacks.Bytes() / 1048576.0, readbacks_peak_bytes / 1048576.0);
                readbacks_queued = readbacks_completed = readbacks_waited = readbacks_canceled = 0;
                readbacks_peak_bytes = pending_readbacks.Bytes();
                readback_sources.Report(stdout);
                if (stale_readbacks) {
                    std::printf("Readback byte rule: %llu CPU writes went on without waiting\n",
                                static_cast<unsigned long long>(readbacks_released));
                    readbacks_released = 0;
                }
                std::ranges::sort(readback_images, std::greater{}, &ReadbackImageStat::copies);
                for (u32 i = 0; i < 4 && readback_images[i].copies; ++i) {
                    const auto& r = readback_images[i];
                    std::printf("Readback image %#llx: %ux%u, %u bits, %s, %u copies, %u CPU accesses "
                                "while pending\n",
                                static_cast<unsigned long long>(r.address), r.width, r.height, r.bits,
                                vk::to_string(r.format).c_str(), r.copies, r.cpu_accesses);
                }
                readback_images = {};
                readback_report = now;
            }
        }
        return;
    }
    std::unique_lock lk{download_images_mutex};
    boost::container::small_vector<PendingDownload, 16> pending;
    for (const ImageId image_id : download_images) {
        if (auto download = RecordImageDownload(image_id)) {
            pending.push_back(*download);
        }
    }
    download_images.clear();
    if (pending.empty()) {
        return;
    }
    scheduler.Finish();
    for (const PendingDownload& download : pending) {
        download.staging.Invalidate();
        Gow3WriteLog::NoteWriteSite(download.guest_address, download.size, 3);
        Core::Memory::Instance()->TryWriteBacking(std::bit_cast<u8*>(download.guest_address),
                                                  download.staging.mapped, download.size);
        Vulkan::SrtNoteReadback(download.guest_address, download.size);
        runtime.GetStagingPool().FreeDeferred(download.staging);
    }
}

std::optional<TextureCache::PendingDownload> TextureCache::RecordImageDownload(ImageId image_id) {
    Image& image = slot_images[image_id];
    if (False(image.flags & ImageFlagBits::GpuModified)) {
        return std::nullopt;
    }
    const u32 download_size = image.info.pitch * image.info.size.height * image.info.size.depth *
                              image.info.resources.layers * (image.info.num_bits / 8);
    ASSERT(download_size <= image.info.guest_size);
    const auto download =
        runtime.GetStagingPool().Request(download_size, MemoryType::HostCached, 16, true);
    const vk::BufferImageCopy image_download = {
        .bufferOffset = download.offset,
        .bufferRowLength = image.info.pitch,
        .bufferImageHeight = image.info.size.height,
        .imageSubresource =
            {
                .aspectMask = image.info.props.is_depth ? vk::ImageAspectFlagBits::eDepth
                                                        : vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
        .imageOffset = {0, 0, 0},
        .imageExtent = {image.info.size.width, image.info.size.height, image.info.size.depth},
    };
    runtime.DownloadImage(&image, download.buffer, std::span{&image_download, 1});
    return PendingDownload{image.info.guest_address, download_size, download};
}

u32 TextureCache::DownloadQueuedImagesNow(VAddr address, u64 size) {
    boost::container::small_vector<ImageId, 4> hits;
    {
        std::unique_lock lk{download_images_mutex};
        for (auto* set : {&download_images, &tiny_download_images}) {
            for (auto it = set->begin(); it != set->end();) {
                const Image& image = slot_images[*it];
                if (image.Overlaps(address, size)) {
                    if (std::ranges::find(hits, *it) == hits.end()) {
                        hits.push_back(*it);
                    }
                    it = set->erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
    for (const ImageId image_id : hits) {
        DownloadImageMemory(image_id, true);
    }
    return static_cast<u32>(hits.size());
}

void TextureCache::DownloadImageMemory(ImageId image_id, bool sync) {
    Image& image = slot_images[image_id];
    ResolveReadbacks(image.info.guest_address, image.info.guest_size, true,
                     ReadbackSource::ImageDownload);
    if (False(image.flags & ImageFlagBits::GpuModified)) {
        return;
    }
    const u32 download_size = image.info.pitch * image.info.size.height * image.info.size.depth *
                              image.info.resources.layers * (image.info.num_bits / 8);
    ASSERT(download_size <= image.info.guest_size);
    const auto download =
        runtime.GetStagingPool().Request(download_size, MemoryType::HostCached, 16, !sync);
    const vk::BufferImageCopy image_download = {
        .bufferOffset = download.offset,
        .bufferRowLength = image.info.pitch,
        .bufferImageHeight = image.info.size.height,
        .imageSubresource =
            {
                .aspectMask = image.info.props.is_depth ? vk::ImageAspectFlagBits::eDepth
                                                        : vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
        .imageOffset = {0, 0, 0},
        .imageExtent = {image.info.size.width, image.info.size.height, image.info.size.depth},
    };
    runtime.DownloadImage(&image, download.buffer, std::span{&image_download, 1});
    if (sync) {
        scheduler.Finish();
        download.Invalidate();
        Gow3WriteLog::NoteWriteSite(image.info.guest_address, download_size, 4);
        Core::Memory::Instance()->TryWriteBacking(std::bit_cast<u8*>(image.info.guest_address),
                                                  download.mapped, download_size);
        Vulkan::SrtNoteReadback(image.info.guest_address, download_size);
    } else {
        Gow3WriteLog::ClobberSnap(download.mapped, image.info.guest_address, download_size);
        scheduler.DeferPriorityOperation(
            [this, device_addr = image.info.guest_address, download, download_size] {
                download.Invalidate();
                Gow3WriteLog::ClobberCheck(download.mapped, device_addr, download.mapped,
                                           download_size, "image download");
                Gow3WriteLog::NoteWriteSite(device_addr, download_size, 5);
                Core::Memory::Instance()->TryWriteBacking(std::bit_cast<u8*>(device_addr),
                                                          download.mapped, download_size);
                Vulkan::SrtNoteReadback(device_addr, download_size);
                runtime.GetStagingPool().FreeDeferred(download);
            });
    }
}

// gow3: images sharing guest memory (aliases), ported from cuesta4/shadPS4 (eltutz) commit
// 189ca3b6. A write to one is copied into the others before they are read, as on the PS4,
// where they are the same memory.
static bool Covers(const Extent3D& extent, const ImageInfo& info) {
    return extent.width == info.size.width && extent.height == info.size.height &&
           extent.depth == info.size.depth;
}

static bool CanAlias(const Image& lhs, const Image& rhs) {
    return GetAliasCopyExtent(lhs.info, rhs.info) || GetAliasCopyExtent(rhs.info, lhs.info);
}

template <typename Func>
void TextureCache::ForEachAlias(const Image& image, Func&& func) {
    const auto page = page_table.find(image.info.guest_address >> Traits::PAGE_BITS);
    if (page == nullptr) {
        return;
    }
    for (const ImageId candidate_id : *page) {
        Image& candidate = slot_images[candidate_id];
        if (&candidate != &image && candidate.info.guest_address == image.info.guest_address &&
            CanAlias(candidate, image)) {
            func(candidate_id, candidate);
        }
    }
}

void TextureCache::SynchronizeAlias(ImageId image_id) {
    Image& dst = slot_images[image_id];
    if (False(dst.flags & ImageFlagBits::Aliased)) {
        return;
    }
    const auto state_it = alias_states.find(dst.info.guest_address);
    if (state_it == alias_states.end()) {
        return;
    }
    AliasState& state = state_it.value();
    if (!IsLiveImage(state.backing, state.backing_uid)) {
        if (!IsLiveImage(state.writer, state.writer_uid)) {
            state.ResetAuthority();
            return;
        }
        state.backing = state.writer, state.backing_uid = state.writer_uid;
        state.writer = {}, state.writer_uid = 0;
    }
    if (state.writer == image_id && state.writer_uid == dst.image_uid) {
        return;
    }
    if (!CommitAliasWriter(state)) {
        state.ResetAuthority();
        return;
    }
    Image& current = slot_images[state.backing];
    const std::optional extent = GetAliasCopyExtent(current.info, dst.info);
    if (state.backing == image_id || current.alias_generation <= dst.alias_generation || !extent) {
        return;
    }
    CopyAlias(state.backing, image_id, *extent);
    dst.alias_generation = current.alias_generation;
    const std::optional reverse = GetAliasCopyExtent(dst.info, current.info);
    if (reverse && Covers(*reverse, current.info) && !Covers(*extent, dst.info)) {
        state.backing = image_id, state.backing_uid = dst.image_uid;
    }
}

bool TextureCache::CommitAliasWriter(AliasState& state) {
    if (!state.writer) {
        return true;
    }
    if (!IsLiveImage(state.writer, state.writer_uid) ||
        !IsLiveImage(state.backing, state.backing_uid)) {
        return false;
    }
    Image& writer = slot_images[state.writer];
    Image& backing = slot_images[state.backing];
    if (state.writer != state.backing && writer.alias_generation > backing.alias_generation) {
        const std::optional extent = GetAliasCopyExtent(writer.info, backing.info);
        if (!extent) {
            return false;
        }
        CopyAlias(state.writer, state.backing, *extent);
        backing.alias_generation = writer.alias_generation;
    }
    state.writer = {}, state.writer_uid = 0;
    return true;
}

void TextureCache::CopyAlias(ImageId src_id, ImageId dst_id, const Extent3D& extent) {
    Image& dst = slot_images[dst_id];
    runtime.CopyAliasImage(&slot_images[src_id], &dst, {extent.width, extent.height, extent.depth});
    dst.flags |= ImageFlagBits::GpuModified;
    dst.flags &= ~ImageFlagBits::Dirty;
}

void TextureCache::PublishAliasWrite(ImageId image_id) {
    Image& image = slot_images[image_id];
    image.alias_generation = ++alias_generation;
    if (False(image.flags & ImageFlagBits::Aliased)) {
        return;
    }
    std::scoped_lock lock{mutex};
    AliasState& state = alias_states[image.info.guest_address];
    if (IsLiveImage(state.backing, state.backing_uid) &&
        !CanAlias(image, slot_images[state.backing])) {
        state.ResetAuthority();
    }
    state.members = std::max(state.members, 2u);
    if (state.writer == image_id && state.writer_uid == image.image_uid) {
        return;
    }
    if (IsLiveImage(state.backing, state.backing_uid)) {
        const ImageInfo& backing = slot_images[state.backing].info;
        const std::optional extent = GetAliasCopyExtent(image.info, backing);
        if (extent && Covers(*extent, backing)) {
            state.backing = image_id, state.backing_uid = image.image_uid;
        } else {
            state.writer = image_id, state.writer_uid = image.image_uid;
        }
    } else {
        state.backing = image_id, state.backing_uid = image.image_uid;
    }
}

void TextureCache::InvalidateAlias(Image& image) {
    image.alias_generation = 0;
    if (True(image.flags & ImageFlagBits::Aliased)) {
        if (const auto it = alias_states.find(image.info.guest_address); it != alias_states.end()) {
            it.value().ResetAuthority();
        }
    }
}

void TextureCache::MarkAsMaybeDirty(ImageId image_id, Image& image) {
    WatchLog("maybe cpu write", image);
    if (image.hash == 0) {
        // Initialize hash
        const u8* addr = std::bit_cast<u8*>(image.info.guest_address);
        image.hash = XXH3_64bits(addr, image.info.guest_size);
    }
    image.flags |= ImageFlagBits::MaybeCpuDirty;
    UntrackImage(image_id);
}

void TextureCache::InvalidateMemory(VAddr addr, size_t size) {
    std::scoped_lock lock{mutex};
    const auto pages_start = PageManager::GetPageAddr(addr);
    const auto pages_end = PageManager::GetNextPageAddr(addr + size - 1);
    ForEachImageInRegion(pages_start, pages_end - pages_start, [&](ImageId image_id, Image& image) {
        const auto image_begin = image.info.guest_address;
        const auto image_end = image.info.guest_address + image.info.guest_size;
        if (image.Overlaps(addr, size)) {
            // Modified region overlaps image, so the image was definitely accessed by this fault.
            WatchLog("cpu write", image);
            // Untrack the image, so that the range is unprotected and the guest can write freely.
            InvalidateAlias(image);
            image.flags |= ImageFlagBits::CpuDirty;
            UntrackImage(image_id);
        } else if (pages_end < image_end) {
            // This page access may or may not modify the image.
            // We should not mark it as dirty now. If it really was modified
            // it will receive more invalidations on its other pages.
            // Remove tracking from this page only.
            UntrackImageHead(image_id);
        } else if (image_begin < pages_start) {
            // This page access does not modify the image but the page should be untracked.
            // We should not mark this image as dirty now. If it really was modified
            // it will receive more invalidations on its other pages.
            UntrackImageTail(image_id);
        } else {
            // Image begins and ends on this page so it can not receive any more invalidations.
            // We will check it's hash later to see if it really was modified.
            MarkAsMaybeDirty(image_id, image);
        }
    });
}

void TextureCache::InvalidateMemoryFromGPU(VAddr address, size_t max_size) {
    // Partial buffer writes still need the image's untouched bytes in guest memory.
    ResolveReadbacks(address, max_size, true, ReadbackSource::GpuPartialWrite);
    std::scoped_lock lock{mutex};
    ForEachImageInRegion(address, max_size, [&](ImageId image_id, Image& image) {
        // Only consider images that match base address.
        WatchLog("gpu buffer write", image);
        // TODO: Maybe also consider subresources
        if (image.info.guest_address != address) {
            return;
        }
        // Ensure image is reuploaded when accessed again.
        InvalidateAlias(image);
        image.flags |= ImageFlagBits::GpuDirty;
    });
}

void TextureCache::UnmapMemory(VAddr cpu_addr, size_t size) {
    CancelReadbacks(cpu_addr, size);
    std::scoped_lock lk{mutex};

    ImageIds deleted_images;
    ForEachImageInRegion(cpu_addr, size, [&](ImageId id, Image&) { deleted_images.push_back(id); });
    for (const ImageId id : deleted_images) {
        // TODO: Download image data back to host.
        FreeImage(id);
    }
}

ImageId TextureCache::ResolveDepthOverlap(const ImageInfo& requested_info, BindingType binding,
                                          ImageId cache_image_id) {
    auto& cache_image = slot_images[cache_image_id];

    if (!cache_image.info.props.is_depth && !requested_info.props.is_depth) {
        return {};
    }

    const bool stencil_match =
        requested_info.props.has_stencil == cache_image.info.props.has_stencil;
    const bool bpp_match = requested_info.num_bits == cache_image.info.num_bits;

    // If an image in the cache has less slices we need to expand it
    bool recreate = cache_image.info.resources < requested_info.resources;

    switch (binding) {
    case BindingType::Texture:
        // The guest requires a depth sampled texture, but cache can offer only Rxf. Need to
        // recreate the image.
        recreate |= requested_info.props.is_depth && !cache_image.info.props.is_depth;
        break;
    case BindingType::Storage:
        // If the guest is going to use previously created depth as storage, the image needs to be
        // recreated. (TODO: Probably a case with linear rgba8 aliasing is legit)
        recreate |= cache_image.info.props.is_depth;
        break;
    case BindingType::RenderTarget:
        // Render target can have only Rxf format. If the cache contains only Dx[S8] we need to
        // re-create the image.
        ASSERT(!requested_info.props.is_depth);
        recreate |= cache_image.info.props.is_depth;
        break;
    case BindingType::DepthTarget:
        // The guest has requested previously allocated texture to be bound as a depth target.
        // In this case we need to convert Rx float to a Dx[S8] as requested
        recreate |= !cache_image.info.props.is_depth;

        // The guest is trying to bind a depth target and cache has it. Need to be sure that aspects
        // and bpp match
        recreate |= cache_image.info.props.is_depth && !(stencil_match && bpp_match);
        break;
    default:
        break;
    }

    if (recreate) {
        const bool fresh = ExpandFresh();
        auto new_info = requested_info;
        if (fresh) {
            // SubresourceExtent compares levels first, so std::max can drop layers.
            new_info.resources.levels =
                std::max(requested_info.resources.levels, cache_image.info.resources.levels);
            new_info.resources.layers =
                std::max(requested_info.resources.layers, cache_image.info.resources.layers);
        } else {
            new_info.resources = std::max(requested_info.resources, cache_image.info.resources);
        }
        new_info.UpdateSize();
        const auto new_image_id = slot_images.Insert(instance, runtime, slot_image_views, new_info);
        RegisterImage(new_image_id);

        // Inherit image usage
        auto& new_image = slot_images[new_image_id];
        new_image.usage = cache_image.usage;
        const bool src_gpu = True(cache_image.flags & ImageFlagBits::GpuModified);
        const u32 src_flags = static_cast<u32>(cache_image.flags);
        const u64 uploaded = Gow3Stats::image_upload_bytes.load(std::memory_order_relaxed);
        if (fresh && src_gpu) {
            RefreshImage(cache_image);
        }
        // Upstream #5229: the added layers/levels start from guest memory, not undefined.
        if (new_info.num_samples == 1 &&
            ((fresh && !src_gpu) ||
             new_info.resources.layers > cache_image.info.resources.layers ||
             new_info.resources.levels > cache_image.info.resources.levels)) {
            RefreshImage(new_image);
        }
        const bool refreshed =
            Gow3Stats::image_upload_bytes.load(std::memory_order_relaxed) != uploaded;
        new_image.flags &= ~ImageFlagBits::Dirty;
        // When creating a depth buffer through overlap resolution don't clear it on first use.
        new_image.info.meta_info.htile_clear_mask = 0;
        const VAddr old_htile = cache_image.info.meta_info.htile_addr;
        bool had_meta = false;
        s32 old_clear_mask = 0;
        if (old_htile) {
            const auto it = surface_metas.find(old_htile);
            if (it != surface_metas.end() && it.value().type == MetaType::HTile &&
                it.value().owner_uid == cache_image.image_uid) {
                had_meta = true;
                old_clear_mask = it.value().clear_mask;
            }
        }
        const bool keep_clear = DepthKeepClear() && had_meta;
        if (keep_clear) {
            new_image.info.meta_info.htile_addr = old_htile;
            new_image.info.meta_info.htile_clear_mask = old_clear_mask;
        }
        const bool copied = !fresh || src_gpu;
        if (copied) {
            runtime.CopyColorAndDepth(&cache_image, &new_image);
        }
        if (ExpandTrace() && ExpandTraceBudget()) {
            const auto& o = cache_image.info;
            std::printf("Expand trace: %.2f s recreate (depth) addr %#llx size %#llx type %d fmt %d "
                        "%ux%u layers %u->%u levels %u->%u flags %#x gpu %d bound %d target %d "
                        "refreshed %d copied %d htile %#llx meta %d clear_mask %#x kept %d\n",
                        ExpandTraceTime(), (unsigned long long)o.guest_address,
                        (unsigned long long)new_info.guest_size, int(new_info.type),
                        int(new_info.pixel_format), new_info.size.width, new_info.size.height,
                        u32(o.resources.layers), u32(new_info.resources.layers),
                        u32(o.resources.levels), u32(new_info.resources.levels), src_flags,
                        int(src_gpu), int(cache_image.binding.is_bound),
                        int(cache_image.binding.is_target), int(refreshed), int(copied),
                        (unsigned long long)old_htile, int(had_meta), u32(old_clear_mask),
                        int(keep_clear));
        }

        // Free the cache image.
        FreeImage(cache_image_id);
        if (keep_clear) {
            const auto& kept = slot_images[new_image_id];
            RegisterMeta(old_htile, MetaDataInfo{
                                        .type = MetaType::HTile,
                                        .owner_id = new_image_id,
                                        .owner_uid = kept.image_uid,
                                        .num_samples = kept.info.num_samples,
                                        .clear_mask = old_clear_mask,
                                    });
        }
        return new_image_id;
    }

    // Will be handled by view
    return cache_image_id;
}

// ucndc: GOW3_MIP_FIX_TRACE=1 (diagnostic, off): how the images MIP_FIX gives mips to are made
// and used. One line per image the first time it gets a mip through MIP_FIX, one per target
// dropped for a larger image, and one per mip the first time it is used while its contents are
// undefined (layout UNDEFINED with no upload pending).
static bool MipFixTrace() {
    static const bool on = std::getenv("GOW3_MIP_FIX_TRACE") != nullptr;
    return on;
}
static bool MipFixTraceBudget() {
    static std::atomic<int> lines{0};
    return lines.fetch_add(1, std::memory_order_relaxed) < 2000;
}
static double MipFixTraceTime() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}
/// The first time `key` is seen (image uid, or uid * 16 + mip).
static bool MipFixFirst(u64 key) {
    static std::mutex mutex;
    static std::unordered_set<u64> seen;
    std::scoped_lock lock{mutex};
    return seen.insert(key).second;
}
static vk::ImageLayout MipLayout(const Image& image, u32 mip) {
    const auto& states = image.backing->subresource_states;
    const u32 index = mip * image.info.resources.layers;
    return index < states.size() ? states[index].layout : image.backing->state.layout;
}
static std::string MipLayouts(const Image& image) {
    std::string out;
    for (u32 m = 0; m < image.info.resources.levels && m < 14; ++m) {
        out += m ? "," : "";
        out += vk::to_string(MipLayout(image, m));
    }
    return out;
}

std::tuple<ImageId, int, int> TextureCache::ResolveOverlap(const ImageInfo& image_info,
                                                           BindingType binding,
                                                           ImageId cache_image_id,
                                                           ImageId merged_image_id) {
    static constexpr u64 NUM_FRAMES_BEFORE_REMOVAL = 32;

    auto& cache_image = slot_images[cache_image_id];
    const bool safe_to_delete =
        scheduler.CurrentTick() - cache_image.tick_accessed_last > NUM_FRAMES_BEFORE_REMOVAL;

    // Equal address
    if (image_info.guest_address == cache_image.info.guest_address) {
        const u32 lhs_block_size = image_info.num_bits * image_info.num_samples;
        const u32 rhs_block_size = cache_image.info.num_bits * cache_image.info.num_samples;
        if (image_info.BlockDim() != cache_image.info.BlockDim() ||
            lhs_block_size != rhs_block_size) {
            // Very likely this kind of overlap is caused by allocation from a pool.
            if (safe_to_delete) {
                FreeImage(cache_image_id);
            }
            return {merged_image_id, -1, -1};
        }

        if (const auto depth_image_id = ResolveDepthOverlap(image_info, binding, cache_image_id)) {
            return {depth_image_id, -1, -1};
        }

        // Compressed view of uncompressed image with same block size.
        if (image_info.props.is_block && !cache_image.info.props.is_block) {
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        if (image_info.guest_size == cache_image.info.guest_size &&
            (image_info.type == AmdGpu::ImageType::Color3D ||
             cache_image.info.type == AmdGpu::ImageType::Color3D)) {
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        const bool pow2_padding_only =
            image_info.props.is_pow2 != cache_image.info.props.is_pow2 &&
            image_info.tile_mode == cache_image.info.tile_mode &&
            image_info.size == cache_image.info.size &&
            image_info.pitch == cache_image.info.pitch && image_info.resources.levels == 1 &&
            cache_image.info.resources.levels == 1 && image_info.resources.layers == 1 &&
            cache_image.info.resources.layers == 1;

        // Size and resources are less than or equal, use image view.
        if (image_info.pixel_format != cache_image.info.pixel_format ||
            image_info.guest_size <= cache_image.info.guest_size || pow2_padding_only) {
            auto result_id = merged_image_id ? merged_image_id : cache_image_id;
            const auto& result_image = slot_images[result_id];
            const bool is_compatible =
                IsVulkanFormatCompatible(result_image.info.pixel_format, image_info.pixel_format);
            return {is_compatible ? result_id : ImageId{}, -1, -1};
        }

        // Size and resources are greater, expand the image.
        if (image_info.type == cache_image.info.type &&
            image_info.resources > cache_image.info.resources) {
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        // Size is greater but resources are not, because the tiling mode is different.
        // Likely the address is reused for a image with a different tiling mode.
        if (image_info.tile_mode != cache_image.info.tile_mode) {
            if (safe_to_delete) {
                FreeImage(cache_image_id);
            }
            return {merged_image_id, -1, -1};
        }

        // Enhanced debug logging for unreachable case
        // Calculate expected size based on format and dimensions
        u64 expected_size =
            (static_cast<u64>(image_info.size.width) * static_cast<u64>(image_info.size.height) *
             static_cast<u64>(image_info.size.depth) * static_cast<u64>(image_info.num_bits) / 8);
        LOG_ERROR(Render_Vulkan,
                  "Unresolvable image overlap with equal memory address:\n"
                  "=== OLD IMAGE (cached) ===\n"
                  "  Address:        {:#x}\n"
                  "  Size:           {:#x} bytes\n"
                  "  Format:         {}\n"
                  "  Type:           {}\n"
                  "  Width:          {}\n"
                  "  Height:         {}\n"
                  "  Depth:          {}\n"
                  "  Pitch:          {}\n"
                  "  Mip levels:     {}\n"
                  "  Array layers:   {}\n"
                  "  Samples:        {}\n"
                  "  Tile mode:      {:#x}\n"
                  "  Block size:     {} bits\n"
                  "  Is block-comp:  {}\n"
                  "  Guest size:     {:#x}\n"
                  "  Last accessed:  tick {}\n"
                  "  Safe to delete: {}\n"
                  "  isPow2:         {}\n"
                  "  Alt tile:       {}\n"
                  "\n"
                  "=== NEW IMAGE (requested) ===\n"
                  "  Address:        {:#x}\n"
                  "  Size:           {:#x} bytes\n"
                  "  Format:         {}\n"
                  "  Type:           {}\n"
                  "  Width:          {}\n"
                  "  Height:         {}\n"
                  "  Depth:          {}\n"
                  "  Pitch:          {}\n"
                  "  Mip levels:     {}\n"
                  "  Array layers:   {}\n"
                  "  Samples:        {}\n"
                  "  Tile mode:      {:#x}\n"
                  "  Block size:     {} bits\n"
                  "  Is block-comp:  {}\n"
                  "  Guest size:     {:#x}\n"
                  "  isPow2:         {}\n"
                  "  Alt tile:       {}\n"
                  "\n"
                  "=== COMPARISON ===\n"
                  "  Same format:           {}\n"
                  "  Same type:             {}\n"
                  "  Same tile mode:        {}\n"
                  "  Same block size:       {}\n"
                  "  Same BlockDim:         {}\n"
                  "  Same pitch:            {}\n"
                  "  Same pow2:             {}\n"
                  "  Same alt tile:         {}\n"
                  "  Old resources <= new:  {} (old: {}, new: {})\n"
                  "  Old size <= new size:  {}\n"
                  "  Expected size (calc):  {} bytes\n"
                  "  Size ratio (new/expected): {:.2f}x\n"
                  "  Size ratio (new/old):  {:.2f}x\n"
                  "  Old vs expected diff:  {} bytes ({:+.2f}%)\n"
                  "  New vs expected diff:  {} bytes ({:+.2f}%)\n"
                  "  Merged image ID:       {}\n"
                  "  Binding type:          {}\n"
                  "  Current tick:          {}\n"
                  "  Age (ticks since last access): {}",

                  // Old image details
                  cache_image.info.guest_address, cache_image.info.guest_size,
                  vk::to_string(cache_image.info.pixel_format),
                  static_cast<int>(cache_image.info.type), cache_image.info.size.width,
                  cache_image.info.size.height, cache_image.info.size.depth, cache_image.info.pitch,
                  cache_image.info.resources.levels, cache_image.info.resources.layers,
                  cache_image.info.num_samples, static_cast<u32>(cache_image.info.tile_mode),
                  cache_image.info.num_bits, +cache_image.info.props.is_block,
                  cache_image.info.guest_size, cache_image.tick_accessed_last, safe_to_delete,
                  bool(cache_image.info.props.is_pow2), cache_image.info.alt_tile,

                  // New image details
                  image_info.guest_address, image_info.guest_size,
                  vk::to_string(image_info.pixel_format), static_cast<int>(image_info.type),
                  image_info.size.width, image_info.size.height, image_info.size.depth,
                  image_info.pitch, image_info.resources.levels, image_info.resources.layers,
                  image_info.num_samples, static_cast<u32>(image_info.tile_mode),
                  image_info.num_bits, image_info.props.is_block, image_info.guest_size,
                  bool(image_info.props.is_pow2), image_info.alt_tile,

                  // Comparison
                  (image_info.pixel_format == cache_image.info.pixel_format),
                  (image_info.type == cache_image.info.type),
                  (image_info.tile_mode == cache_image.info.tile_mode),
                  (image_info.num_bits == cache_image.info.num_bits),
                  (image_info.BlockDim() == cache_image.info.BlockDim()),
                  (image_info.pitch == cache_image.info.pitch),
                  (image_info.props.is_pow2 == cache_image.info.props.is_pow2),
                  (image_info.alt_tile == cache_image.info.alt_tile),
                  (cache_image.info.resources <= image_info.resources),
                  cache_image.info.resources.levels, image_info.resources.levels,
                  (cache_image.info.guest_size <= image_info.guest_size), expected_size,

                  // Size ratios
                  static_cast<double>(image_info.guest_size) / expected_size,
                  static_cast<double>(image_info.guest_size) / cache_image.info.guest_size,

                  // Difference between actual and expected sizes with percentages
                  static_cast<s64>(cache_image.info.guest_size) - static_cast<s64>(expected_size),
                  (static_cast<double>(cache_image.info.guest_size) / expected_size - 1.0) * 100.0,

                  static_cast<s64>(image_info.guest_size) - static_cast<s64>(expected_size),
                  (static_cast<double>(image_info.guest_size) / expected_size - 1.0) * 100.0,

                  merged_image_id.index, static_cast<int>(binding), scheduler.CurrentTick(),
                  scheduler.CurrentTick() - cache_image.tick_accessed_last);

        UNREACHABLE_MSG("Encountered unresolvable image overlap with equal memory address.");
    }

    // Right overlap, the image requested is a possible subresource of the image from cache.
    if (image_info.guest_address > cache_image.info.guest_address) {
        if (auto mip = image_info.MipOf(cache_image.info); mip >= 0) {
            if (auto slice = image_info.SliceOf(cache_image.info, mip); slice >= 0) {
                return {cache_image_id, mip, slice};
            }
        }

        // ucndc: GOW3_MIP_TRACE=1 (diagnostic): why a target inside an image's range is not taken
        // as one of its mips (the first 40 cases).
        static const bool mip_trace = std::getenv("GOW3_MIP_TRACE") != nullptr;
        static int mip_traces = 0;
        // Bloom (0x1145450000..) and distortion (0x113d6b0000..) chains are always logged.
        const auto in_watch = [](u64 x) {
            return (x >= 0x1145400000ULL && x < 0x1145800000ULL) ||
                   (x >= 0x113d6b0000ULL && x < 0x113d6e0000ULL);
        };
        if (mip_trace && (mip_traces < 40 || in_watch(image_info.guest_address) ||
                          in_watch(cache_image.info.guest_address))) {
            ++mip_traces;
            const auto& a = image_info;
            const auto& b = cache_image.info;
            std::printf("ucndc: mip trace %#llx %ux%u fmt %d bits %u pitch %u tile %d array %d levels %u "
                        "type %d inside %#llx %ux%u fmt %d bits %u pitch %u tile %d array %d levels %u "
                        "type %d compatible %d mip_of %d; parent mips:",
                        (unsigned long long)a.guest_address, a.size.width, a.size.height,
                        int(a.pixel_format), a.num_bits, a.pitch, int(a.tile_mode), int(a.array_mode),
                        a.resources.levels, int(a.type), (unsigned long long)b.guest_address,
                        b.size.width, b.size.height, int(b.pixel_format), b.num_bits, b.pitch,
                        int(b.tile_mode), int(b.array_mode), b.resources.levels, int(b.type),
                        int(a.IsCompatible(b)), a.MipOf(b));
            for (u32 m = 0; m < b.resources.levels && m < 8; ++m) {
                const auto& [mip_size, mip_pitch, mip_height, mip_ofs] = b.mips_layout[m];
                std::printf(" [%u +%#llx size %#llx pitch %u h %u]", m, (unsigned long long)mip_ofs,
                            (unsigned long long)mip_size, u32(mip_pitch), u32(mip_height));
            }
            std::printf("\n");
            std::fflush(stdout);
        }
        // Image isn't a subresource but a chance overlap.
        if (safe_to_delete) {
            FreeImage(cache_image_id);
        }

        return {{}, -1, -1};
    } else {
        // Left overlap, the image from cache is a possible subresource of the image requested
        // ucndc: GOW3_MIP_TRACE=1, the other direction: a new image whose range holds an image
        // already in the cache (logged when that one is not taken as its mip).
        static const bool mip_trace_left = std::getenv("GOW3_MIP_TRACE") != nullptr;
        static int mip_traces_left = 0;
        const auto in_watch_left = [](u64 x) {
            return (x >= 0x1145400000ULL && x < 0x1145800000ULL) ||
                   (x >= 0x113d6b0000ULL && x < 0x113d6e0000ULL);
        };
        if (mip_trace_left &&
            (mip_traces_left < 40 || in_watch_left(image_info.guest_address) ||
             in_watch_left(cache_image.info.guest_address)) &&
            cache_image.info.MipOf(image_info) < 0) {
            ++mip_traces_left;
            const auto& a = cache_image.info;
            const auto& b = image_info;
            std::printf("ucndc: mip trace (left) %#llx %ux%u fmt %d bits %u pitch %u tile %d array %d levels %u "
                        "type %d inside %#llx %ux%u fmt %d bits %u pitch %u tile %d array %d levels %u "
                        "type %d compatible %d mip_of %d; parent mips:",
                        (unsigned long long)a.guest_address, a.size.width, a.size.height,
                        int(a.pixel_format), a.num_bits, a.pitch, int(a.tile_mode), int(a.array_mode),
                        a.resources.levels, int(a.type), (unsigned long long)b.guest_address,
                        b.size.width, b.size.height, int(b.pixel_format), b.num_bits, b.pitch,
                        int(b.tile_mode), int(b.array_mode), b.resources.levels, int(b.type),
                        int(a.IsCompatible(b)), a.MipOf(b));
            for (u32 m = 0; m < b.resources.levels && m < 8; ++m) {
                const auto& [mip_size, mip_pitch, mip_height, mip_ofs] = b.mips_layout[m];
                std::printf(" [%u +%#llx size %#llx pitch %u h %u]", m, (unsigned long long)mip_ofs,
                            (unsigned long long)mip_size, u32(mip_pitch), u32(mip_height));
            }
            std::printf("\n");
            std::fflush(stdout);
        }
        if (auto mip = cache_image.info.MipOf(image_info); mip >= 0) {
            if (auto slice = cache_image.info.SliceOf(image_info, mip); slice >= 0) {
                // We have a larger image created and a separate one, representing a subres of it
                // bound as render target. In this case we need to rebind render target.
                if (cache_image.binding.is_target) {
                    if (MipFixTrace() && MipFixTraceBudget()) {
                        std::printf("MIP_FIX trace: %.2f s target %#llx %ux%u fmt %s levels %u (mip %d of a new image) "
                                    "dropped WITHOUT copying its contents, flags %#x\n",
                                    MipFixTraceTime(), (unsigned long long)cache_image.info.guest_address,
                                    cache_image.info.size.width, cache_image.info.size.height,
                                    vk::to_string(cache_image.info.pixel_format).c_str(),
                                    u32(cache_image.info.resources.levels), mip, u32(cache_image.flags));
                    }
                    cache_image.binding.needs_rebind = 1u;
                    if (merged_image_id) {
                        GetImage(merged_image_id).binding.is_target = 1u;
                    }

                    FreeImage(cache_image_id);
                    return {merged_image_id, -1, -1};
                }

                // We need to have a larger, already allocated image to copy this one into
                if (merged_image_id) {
                    auto& merged_image = slot_images[merged_image_id];
                    runtime.CopyMip(&cache_image, &merged_image, mip, slice);
                    FreeImage(cache_image_id);
                }
            }
        }
    }

    return {merged_image_id, -1, -1};
}

void TextureCache::TraceFree(const Image& image, u32 line) {
    if (image.info.resources.layers <= 1 || !ExpandTrace() || !ExpandTraceBudget()) {
        return;
    }
    const auto& o = image.info;
    std::printf("Expand trace: %.2f s free line %u addr %#llx size %#llx type %d fmt %d %ux%u "
                "layers %u levels %u flags %#x bound %d target %d age %llu\n",
                ExpandTraceTime(), line, (unsigned long long)o.guest_address,
                (unsigned long long)o.guest_size, int(o.type), int(o.pixel_format), o.size.width,
                o.size.height, u32(o.resources.layers), u32(o.resources.levels),
                static_cast<u32>(image.flags), int(image.binding.is_bound),
                int(image.binding.is_target),
                (unsigned long long)(scheduler.CurrentTick() - image.tick_accessed_last));
}

ImageId TextureCache::ExpandImage(const ImageInfo& info, ImageId image_id,
                                  std::source_location loc) {
    const auto new_image_id = slot_images.Insert(instance, runtime, slot_image_views, info);
    RegisterImage(new_image_id);

    auto& src_image = slot_images[image_id];
    auto& new_image = slot_images[new_image_id];

    const bool fresh = ExpandFresh();
    const bool src_gpu = True(src_image.flags & ImageFlagBits::GpuModified);
    const u32 src_flags = static_cast<u32>(src_image.flags);
    const u64 uploaded = Gow3Stats::image_upload_bytes.load(std::memory_order_relaxed);
    if (fresh && src_gpu) {
        RefreshImage(src_image);
    }
    RefreshImage(new_image);
    const bool refreshed =
        Gow3Stats::image_upload_bytes.load(std::memory_order_relaxed) != uploaded;
    const bool copied = !fresh || src_gpu;
    if (copied) {
        runtime.CopyImage(&src_image, &new_image);
    }
    if (ExpandTrace() && ExpandTraceBudget()) {
        const auto& o = src_image.info;
        std::printf("Expand trace: %.2f s expand line %u addr %#llx size %#llx->%#llx type %d->%d "
                    "fmt %d->%d %ux%u layers %u->%u levels %u->%u flags %#x gpu %d bound %d "
                    "target %d refreshed %d copied %d\n",
                    ExpandTraceTime(), u32(loc.line()), (unsigned long long)o.guest_address,
                    (unsigned long long)o.guest_size, (unsigned long long)info.guest_size,
                    int(o.type), int(info.type), int(o.pixel_format), int(info.pixel_format),
                    info.size.width, info.size.height, u32(o.resources.layers),
                    u32(info.resources.layers), u32(o.resources.levels),
                    u32(info.resources.levels), src_flags, int(src_gpu),
                    int(src_image.binding.is_bound), int(src_image.binding.is_target),
                    int(refreshed), int(copied));
    }

    if (src_image.binding.is_bound || src_image.binding.is_target) {
        src_image.binding.needs_rebind = 1u;
    }

    FreeImage(image_id);
    TrackImage(new_image_id);
    return new_image_id;
}

ImageId TextureCache::FindImage(ImageDesc& desc, bool exact_fmt) {
    const auto& info = desc.info;
    ASSERT(info.guest_address != 0);

    std::scoped_lock lock{mutex};

    u64 key_hash = info.guest_address ^ info.guest_size << 17 ^ u64(info.pixel_format) << 40 ^
                   u64(info.size.width) << 24 ^ info.size.height ^ u64(desc.type) << 58 ^
                   u64(exact_fmt) << 63;
    key_hash = (key_hash ^ key_hash >> 33) * 0xFF51AFD7ED558CCDull;
    key_hash = (key_hash ^ key_hash >> 33) * 0xC4CEB9FE1A85EC53ull;
    key_hash ^= key_hash >> 33;
    auto& cached = find_image_cache[key_hash % find_image_cache.size()];
    // An exact match stays valid while that image is registered with the same description;
    // resolved overlaps (views into other images) only until any image registration changes.
    const auto still_exact = [&] {
        if (cached.view_mip >= 0 || cached.view_slice >= 0 || !cached.image_id) {
            return false;
        }
        const Image& image = slot_images[cached.image_id];
        return True(image.flags & ImageFlagBits::Registered) &&
               image.info.guest_address == info.guest_address &&
               image.info.guest_size == info.guest_size && image.info.size == info.size &&
               image.info.pixel_format == info.pixel_format;
    };
    if ((cached.generation == registry_generation.load(std::memory_order_relaxed) || still_exact()) &&
        cached.address == info.guest_address &&
        cached.size == info.guest_size && cached.extent == info.size &&
        cached.format == info.pixel_format && cached.type == info.type &&
        cached.exact_fmt == exact_fmt && cached.binding == desc.type &&
        cached.levels == info.resources.levels && cached.layers == info.resources.layers &&
        !Gow3Toggle::Disabled(Gow3Toggle::FindImageCache)) {
        Image& image = slot_images[cached.image_id];
        image.tick_accessed_last = scheduler.CurrentTick();
        TouchImage(image);
        if (cached.view_mip > 0) {
            desc.view_info.range.base.level = cached.view_mip;
        }
        if (cached.view_slice > 0) {
            desc.view_info.range.base.layer = cached.view_slice;
        }
        return cached.image_id;
    }

    ImageIds image_ids;
    ForEachImageInRegion(info.guest_address, info.guest_size,
                         [&](ImageId image_id, Image& image) { image_ids.push_back(image_id); });

    ImageId image_id{};

    // Check for a perfect match first
    for (const auto& cache_id : image_ids) {
        auto& cache_image = slot_images[cache_id];
        if (cache_image.info.guest_address != info.guest_address) {
            continue;
        }
        if (cache_image.info.guest_size != info.guest_size) {
            continue;
        }
        if (cache_image.info.size != info.size) {
            continue;
        }
        if (!IsVulkanFormatCompatible(cache_image.info.pixel_format, info.pixel_format) ||
            (cache_image.info.type != info.type && info.size != Extent3D{1, 1, 1})) {
            continue;
        }
        if (exact_fmt && info.pixel_format != cache_image.info.pixel_format) {
            continue;
        }
        image_id = cache_id;
    }

    // Try to resolve overlaps (if any)
    int view_mip{-1};
    int view_slice{-1};

    // ucndc: GOW3_MIP_FIX=1: a single-level image that is exactly one mip of a larger image in
    // the cache (it was rendered on its own, often before the larger one existed) is copied into
    // that mip and dropped, so the draw writes the larger image's mip and samplers see it.
    if (image_id && info.resources.levels == 1 && ImageInfo::MipFixEnabled()) {
        for (const auto& cache_id : image_ids) {
            if (cache_id == image_id) {
                continue;
            }
            auto& parent = slot_images[cache_id];
            if (parent.info.resources.levels <= 1 ||
                parent.info.guest_address > info.guest_address) {
                continue;
            }
            const s32 mip = info.MipOf(parent.info);
            if (mip < 0) {
                continue;
            }
            const s32 slice = info.SliceOf(parent.info, mip);
            if (slice < 0) {
                continue;
            }
            auto& single = slot_images[image_id];
            // ucndc: GOW3_MIP_FIX_TRACE (see MipFixTrace).
            if (MipFixTrace()) {
                if (MipFixFirst(parent.image_uid << 1) && MipFixTraceBudget()) {
                    std::printf("MIP_FIX trace: %.2f s parent %#llx %ux%u size %#llx fmt %s (vk %s) levels %u layers %u "
                                "flags %#x dirty %d gpu %d target %d bound %d mips [%s]; gets mip %d slice %d from "
                                "%#llx %ux%u pitch %u fmt %s flags %#x target %d\n",
                                MipFixTraceTime(), (unsigned long long)parent.info.guest_address,
                                parent.info.size.width, parent.info.size.height,
                                (unsigned long long)parent.info.guest_size,
                                vk::to_string(parent.info.pixel_format).c_str(),
                                vk::to_string(instance.GetSupportedFormat(parent.info.pixel_format,
                                                                          parent.format_features))
                                    .c_str(),
                                u32(parent.info.resources.levels), u32(parent.info.resources.layers),
                                u32(parent.flags), int(True(parent.flags & ImageFlagBits::Dirty)),
                                int(True(parent.flags & ImageFlagBits::GpuModified)),
                                int(parent.binding.is_target), int(parent.binding.is_bound),
                                MipLayouts(parent).c_str(), mip, slice,
                                (unsigned long long)single.info.guest_address, single.info.size.width,
                                single.info.size.height, single.info.pitch,
                                vk::to_string(single.info.pixel_format).c_str(), u32(single.flags),
                                int(single.binding.is_target));
                }
            }
            runtime.CopyMip(&single, &parent, mip, slice);
            if (single.binding.is_bound || single.binding.is_target) {
                single.binding.needs_rebind = 1u;
            }
            FreeImage(image_id);
            image_id = cache_id;
            view_mip = mip;
            view_slice = slice;
            break;
        }
    }

    if (!image_id) {
        // ucndc: GOW3_OVERLAP_KEEP_VIEW=1 (fix, off; launcher turns it on): a later image in the
        // region that only overlaps by chance (it gives back the image already found, or none)
        // no longer drops the mip/slice found for that image. Before, a target at the address of
        // a mip (UC3 bloom mips 1 and 2) resolved to mip 0 of its image whenever an unrelated
        // image (UC3: a 512x512 table at 0x1145620000) shared the region after it, so the mip
        // was written into the corner of mip 0 and the real mip kept an old frame (red spots).
        static const bool keep_view = [] {
            const char* v = std::getenv("GOW3_OVERLAP_KEEP_VIEW");
            return v && v[0] == '1';
        }();
        for (const auto& cache_id : image_ids) {
            const ImageId found_id = image_id;
            const int found_mip = view_mip, found_slice = view_slice;
            view_mip = -1;
            view_slice = -1;

            const auto& merged_info = image_id ? slot_images[image_id].info : info;
            auto [overlap_image_id, overlap_view_mip, overlap_view_slice] =
                ResolveOverlap(merged_info, desc.type, cache_id, image_id);
            if (overlap_image_id) {
                image_id = overlap_image_id;
                view_mip = overlap_view_mip;
                view_slice = overlap_view_slice;
            }
            if (keep_view && found_id && image_id == found_id && view_mip < 0 && view_slice < 0) {
                view_mip = found_mip;
                view_slice = found_slice;
            }
        }
    }

    if (image_id) {
        Image& image_resolved = slot_images[image_id];
        if (exact_fmt && info.pixel_format != image_resolved.info.pixel_format) {
            // Cannot reuse this image as we need the exact requested format.
            image_id = {};
        } else if (image_resolved.info.resources < info.resources) {
            // The image was clearly picked up wrong.
            FreeImage(image_id);
            image_id = {};
            LOG_WARNING(Render_Vulkan, "Image overlap resolve failed");
        }
    }
    // Create and register a new image
    if (!image_id) {
        image_id = slot_images.Insert(instance, runtime, slot_image_views, info);
        RegisterImage(image_id);
    }

    Image& image = slot_images[image_id];
    image.tick_accessed_last = scheduler.CurrentTick();
    TouchImage(image);

    // If the image requested is a subresource of the image from cache record its location.
    if (view_mip > 0) {
        desc.view_info.range.base.level = view_mip;
    }
    if (view_slice > 0) {
        desc.view_info.range.base.layer = view_slice;
    }

    // ucndc: GOW3_MIP_FIX_TRACE: a mip of a multi-level image used while its contents are
    // undefined (no upload pending).
    if (MipFixTrace() && ImageInfo::MipFixEnabled() &&
        image.info.resources.levels > 1 && False(image.flags & ImageFlagBits::Dirty)) {
        const u32 first = view_mip > 0 ? u32(view_mip) : 0u;
        const u32 end = std::min<u32>(image.info.resources.levels, first + info.resources.levels);
        for (u32 m = first; m < end; ++m) {
            if (MipLayout(image, m) != vk::ImageLayout::eUndefined) {
                continue;
            }
            if (MipFixFirst(((image.image_uid * 16 + m) << 1) | 1) && MipFixTraceBudget()) {
                std::printf("MIP_FIX trace: %.2f s mip %u of %#llx %ux%u fmt %s levels %u used as %s while "
                            "UNDEFINED (flags %#x, mips [%s])\n",
                            MipFixTraceTime(), m, (unsigned long long)image.info.guest_address,
                            image.info.size.width, image.info.size.height,
                            vk::to_string(image.info.pixel_format).c_str(),
                            u32(image.info.resources.levels),
                            desc.type == BindingType::Texture ? "texture" :
                            desc.type == BindingType::Storage ? "storage" : "target",
                            u32(image.flags), MipLayouts(image).c_str());
            }
        }
    }

    // ucndc: GOW3_MIP_TRACE=1: requests in the bloom and distortion chains and what they resolve to.
    static const bool find_trace = std::getenv("GOW3_MIP_TRACE") != nullptr;
    static int find_traces = 0;
    if (find_trace && find_traces < 400 &&
        ((info.guest_address >= 0x1145400000ULL && info.guest_address < 0x1145800000ULL) ||
         (info.guest_address >= 0x113d6b0000ULL && info.guest_address < 0x113d6e0000ULL))) {
        ++find_traces;
        const auto& r = image.info;
        std::printf("ucndc: mip find %#llx %ux%u pitch %u levels %u size %#llx pow2 %d binding %d -> "
                    "%#llx %ux%u pitch %u levels %u size %#llx view_mip %d mips:",
                    (unsigned long long)info.guest_address, info.size.width, info.size.height,
                    info.pitch, info.resources.levels, (unsigned long long)info.guest_size,
                    int(info.props.is_pow2), int(desc.type), (unsigned long long)r.guest_address,
                    r.size.width, r.size.height, r.pitch, r.resources.levels,
                    (unsigned long long)r.guest_size, view_mip);
        for (u32 m = 0; m < r.resources.levels && m < 8; ++m) {
            std::printf(" [%u +%#llx p %u]", m, (unsigned long long)r.mips_layout[m].offset,
                        u32(r.mips_layout[m].pitch));
        }
        std::printf("\n");
        std::fflush(stdout);
    }

    cached = FindImageCacheEntry{
        .address = info.guest_address,
        .size = info.guest_size,
        .extent = info.size,
        .format = info.pixel_format,
        .type = info.type,
        .exact_fmt = exact_fmt,
        .binding = desc.type,
        .levels = info.resources.levels,
        .layers = info.resources.layers,
        .generation = registry_generation.load(std::memory_order_relaxed),
        .image_id = image_id,
        .view_mip = view_mip,
        .view_slice = view_slice,
    };
    return image_id;
}

ImageId TextureCache::FindImageFromRange(VAddr address, size_t size, bool ensure_valid) {
    ImageIds image_ids;
    ForEachImageInRegion(address, size, [&](ImageId image_id, Image& image) {
        if (image.info.guest_address != address) {
            return;
        }
        if (ensure_valid && !image.SafeToDownload()) {
            return;
        }
        image_ids.push_back(image_id);
    });
    if (image_ids.size() == 1) {
        // Sometimes image size might not exactly match with requested buffer size
        // If we only found 1 candidate image use it without too many questions.
        return image_ids.back();
    }
    if (!image_ids.empty()) {
        for (s32 i = 0; i < image_ids.size(); ++i) {
            Image& image = slot_images[image_ids[i]];
            if (image.info.guest_size == size) {
                return image_ids[i];
            }
        }
        LOG_WARNING(Render_Vulkan,
                    "Failed to find exact image match for copy addr={:#x}, size={:#x}", address,
                    size);
    }
    return {};
}

ImageView& TextureCache::FindTexture(ImageId image_id, const ImageDesc& desc, ViewMemo* memo,
                                     bool refresh) {
    Image& image = slot_images[image_id];
    CheckSkippedTarget(image, desc.type == BindingType::Storage);
    if (desc.type == BindingType::Storage) {
        image.flags |= ImageFlagBits::GpuModified;
        if (readback_linear_images && (!image.info.props.is_tiled || image.info.size.width <= 8) &&
            image.info.guest_address != 0) {
            std::unique_lock lk{download_images_mutex};
            download_images.emplace(image_id);
        } else if (TinyReadback(image)) {
            std::unique_lock lk{download_images_mutex};
            tiny_download_images.emplace(image_id);
        }
    }
    if (refresh) {
        UpdateImage(image_id);
    }
    if (desc.type == BindingType::Storage) {
        PublishAliasWrite(image_id);
    }
    if (memo && !Gow3Toggle::Disabled(Gow3Toggle::TextureViewMemo)) {
        if (memo->image_id == image_id && memo->backing == image.backing && memo->view_id) {
            return slot_image_views[memo->view_id];
        }
        ImageView& view = image.FindView(desc.view_info);
        const auto& ids = image.backing->image_view_ids;
        const u32 last = image.backing->last_view;
        memo->image_id = image_id;
        memo->backing = image.backing;
        memo->view_id = last < ids.size() && &slot_image_views[ids[last]] == &view ? ids[last]
                                                                                    : ids.back();
        return view;
    }
    return image.FindView(desc.view_info);
}

ImageView& TextureCache::FindRenderTarget(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    WatchLog("render target", image);
    CheckSkippedTarget(image, true);
    image.flags |= ImageFlagBits::GpuModified;
    if (readback_linear_images && (!image.info.props.is_tiled || image.info.size.width <= 8)) {
        std::unique_lock lk{download_images_mutex};
        download_images.emplace(image_id);
    } else if (TinyReadback(image)) {
        std::unique_lock lk{download_images_mutex};
        tiny_download_images.emplace(image_id);
    }
    image.usage.render_target = 1u;
    UpdateImage(image_id);
    PublishAliasWrite(image_id);

    // Register meta data for this color buffer (upstream #5046: owned, with the sample count)
    const VAddr cmask_addr = desc.info.meta_info.cmask_addr;
    if (image.info.meta_info.cmask_addr != cmask_addr) {
        UnregisterMeta(image.info.meta_info.cmask_addr, image.image_uid);
        image.info.meta_info.cmask_addr = cmask_addr;
    }
    if (cmask_addr) {
        RegisterMeta(cmask_addr, MetaDataInfo{
                                     .type = MetaType::CMask,
                                     .owner_id = image_id,
                                     .owner_uid = image.image_uid,
                                     .num_samples = image.info.num_samples,
                                 });
    }

    const VAddr fmask_addr = desc.info.meta_info.fmask_addr;
    if (image.info.meta_info.fmask_addr != fmask_addr) {
        UnregisterMeta(image.info.meta_info.fmask_addr, image.image_uid);
        image.info.meta_info.fmask_addr = fmask_addr;
    }
    if (fmask_addr) {
        RegisterMeta(fmask_addr, MetaDataInfo{
                                     .type = MetaType::FMask,
                                     .owner_id = image_id,
                                     .owner_uid = image.image_uid,
                                     .num_samples = image.info.num_samples,
                                 });
    }

    return image.FindView(desc.view_info, false);
}

ImageView& TextureCache::FindDepthTarget(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    image.flags |= ImageFlagBits::GpuModified;
    image.usage.depth_target = 1u;
    UpdateImage(image_id);

    // Register meta data for this depth buffer (upstream #5046: owned, with the sample count)
    const VAddr htile_addr = desc.info.meta_info.htile_addr;
    if (image.info.meta_info.htile_addr != htile_addr) {
        UnregisterMeta(image.info.meta_info.htile_addr, image.image_uid);
        image.info.meta_info.htile_addr = htile_addr;
    }
    if (htile_addr) {
        RegisterMeta(htile_addr, MetaDataInfo{
                                     .type = MetaType::HTile,
                                     .owner_id = image_id,
                                     .owner_uid = image.image_uid,
                                     .num_samples = image.info.num_samples,
                                     .clear_mask = image.info.meta_info.htile_clear_mask,
                                 });
    }

    AssociateStencil(image_id, desc.info);

    return image.FindView(desc.view_info, false);
}

void TextureCache::RegisterMeta(VAddr address, MetaDataInfo info) {
    const auto it = surface_metas.find(address);
    if (it == surface_metas.end()) {
        surface_metas.emplace(address, info);
        return;
    }
    auto& registered = it.value();
    if (registered.type != info.type || registered.owner_uid != info.owner_uid) {
        registered = info;
        return;
    }
    // Preserve the current clear state when the same render target is rebound.
    registered.owner_id = info.owner_id;
    registered.num_samples = info.num_samples;
}

void TextureCache::UnregisterMeta(VAddr address, u64 owner_uid) {
    const auto it = surface_metas.find(address);
    if (it != surface_metas.end() && it.value().owner_uid == owner_uid) {
        surface_metas.erase(address);
    }
}

// Upstream #5296: also used by DepthStencilCopy, whose destination is never bound as a target.
void TextureCache::AssociateStencil(ImageId depth_id, const ImageInfo& depth_info) {
    if (depth_info.stencil_addr == 0) {
        return;
    }
    ImageId stencil_id{};
    ForEachImageInRegion(
        depth_info.stencil_addr, depth_info.stencil_size, [&](ImageId image_id, Image& image) {
            if (image.info.guest_address != depth_info.stencil_addr) {
                return;
            }
            if (image.info.pixel_format == vk::Format::eUndefined ||
                Vulkan::LiverpoolToVK::IsFormatStencilCompatible(image.info.pixel_format)) {
                stencil_id = image_id;
            }
        });
    if (!stencil_id) {
        ImageInfo info{};
        info.guest_address = depth_info.stencil_addr;
        info.guest_size = depth_info.stencil_size;
        info.size = depth_info.size;
        stencil_id = slot_images.Insert(instance, runtime, slot_image_views, info);
        RegisterImage(stencil_id);
    }
    Image& stencil_image = slot_images[stencil_id];
    TouchImage(stencil_image);
    stencil_image.AssociateDepth(depth_id, slot_images[depth_id].image_uid);
}

void TextureCache::RefreshImage(Image& image) {
    if (False(image.flags & ImageFlagBits::Dirty) || image.info.num_samples > 1) {
        return;
    }
    WatchLog("refresh from guest memory", image);
    ResolveReadbacks(image.info.guest_address, image.info.guest_size, true,
                     ReadbackSource::ImageRefresh);
    Gow3Stats::Timer timer{Gow3Stats::t_refresh};

    RENDERER_TRACE;
    TRACE_HINT(fmt::format("{:x}:{:x}", image.info.guest_address, image.info.guest_size));

    // uc3: GOW3_SMALL_STRICT=1 - diagnostic: re-upload MaybeCpuDirty images instead of trusting
    // the hash (white hair after Continue).
    static const bool small_strict = std::getenv("GOW3_SMALL_STRICT") != nullptr;
    if (!small_strict && True(image.flags & ImageFlagBits::MaybeCpuDirty) &&
        False(image.flags & ImageFlagBits::CpuDirty)) {
        // The image size should be less than page size to be considered MaybeCpuDirty, so the
        // whole image is hashed (uc3: the first 64 pixels alone missed a small texture whose
        // start matched the previous content of the reused memory, and MarkAsMaybeDirty hashes
        // the whole image anyway, so the two hashes never compared the same bytes).
        const auto addr = std::bit_cast<u8*>(image.info.guest_address);
        const u64 hash = XXH3_64bits(addr, image.info.guest_size);
        if (image.hash == hash) {
            image.flags &= ~ImageFlagBits::MaybeCpuDirty;
            return;
        }
        image.hash = hash;
    }

    const u32 num_layers = image.info.resources.layers;
    const u32 num_mips = image.info.resources.levels;
    const bool is_gpu_modified = True(image.flags & ImageFlagBits::GpuModified);
    const bool is_gpu_dirty = True(image.flags & ImageFlagBits::GpuDirty);

    Gow3Stats::image_upload_bytes.fetch_add(image.info.guest_size, std::memory_order_relaxed);
    boost::container::small_vector<vk::BufferImageCopy, 14> image_copies;
    for (u32 m = 0; m < num_mips; m++) {
        const u32 width = std::max(image.info.size.width >> m, 1u);
        const u32 height = std::max(image.info.size.height >> m, 1u);
        const u32 depth =
            image.info.props.is_volume ? std::max(image.info.size.depth >> m, 1u) : 1u;
        const auto [mip_size, mip_pitch, mip_height, mip_offset] = image.info.mips_layout[m];

        // Protect GPU modified resources from accidental CPU reuploads.
        if (is_gpu_modified && !is_gpu_dirty) {
            const u8* addr = std::bit_cast<u8*>(image.info.guest_address);
            const u64 hash = XXH3_64bits(addr + mip_offset, mip_size);
            if (image.mip_hashes[m] == hash) {
                continue;
            }
            image.mip_hashes[m] = hash;
            // uc3: GOW3_FLASH_TRACE=1 — a GPU-written image whose guest bytes changed is
            // re-uploaded from guest memory: for a render target that is a frame of stale
            // (or black) content. Time-stamped so the player can match it to a flash.
            static const bool flash_trace = std::getenv("GOW3_FLASH_TRACE") != nullptr;
            if (flash_trace && image.info.size.width >= 640 && m == 0) {
                static std::atomic<int> shown{0};
                if (shown.fetch_add(1, std::memory_order_relaxed) < 400) {
                    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - uc3_flash_clock).count();
                    std::printf("Flash trace: %.2f s: GPU-written image %ux%u fmt %u at %#llx re-uploaded from guest memory (cpu dirty %d)\n",
                                t, image.info.size.width, image.info.size.height,
                                u32(image.info.pixel_format), (unsigned long long)image.info.guest_address,
                                int(True(image.flags & ImageFlagBits::CpuDirty)));
                }
            }
        }

        const u32 extent_width = mip_pitch ? std::min<u32>(mip_pitch, width) : width;
        const u32 extent_height = mip_height ? std::min<u32>(mip_height, height) : height;
        image_copies.push_back({
            .bufferOffset = mip_offset,
            .bufferRowLength = mip_pitch,
            .bufferImageHeight = mip_height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = m,
                .baseArrayLayer = 0,
                .layerCount = num_layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {extent_width, extent_height, depth},
        });
    }

    if (image_copies.empty()) {
        image.flags &= ~ImageFlagBits::Dirty;
        return;
    }

    scheduler.EndRendering();

    const auto [in_buffer, in_offset] =
        buffer_cache.ObtainBufferForImage(image.info.guest_address, image.info.guest_size);
    const auto [buffer, offset] = tile_manager.DetileImage(in_buffer, in_offset, image.info);
    for (auto& copy : image_copies) {
        copy.bufferOffset += offset;
    }

    runtime.UploadImage(&image, buffer, image_copies);
}

vk::Sampler TextureCache::GetSampler(const AmdGpu::Sampler& sampler,
                                     AmdGpu::BorderColorBuffer border_color_base,
                                     const bool is_depth, float extra_lod_bias) {
    // Compare and plain uses of one S# need separate samplers; so do extra LOD biases.
    const u64 hash = HashCombine(HashCombine(XXH3_64bits(&sampler, sizeof(sampler)), is_depth),
                                 u64(std::bit_cast<u32>(extra_lod_bias)));

    std::scoped_lock lock{samplers_mutex};
    const auto [it, new_sampler] = samplers.try_emplace(hash, instance, sampler, border_color_base,
                                                        is_depth, extra_lod_bias);
    if (new_sampler) {
        samplers.at(hash).lru_id = sampler_lru_cache.Insert(hash, gc_tick);
    } else {
        sampler_lru_cache.Touch(it->second.lru_id, gc_tick);
    }

    return it->second.Handle();
}

void TextureCache::RegisterImage(ImageId image_id) {
    Gow3Stats::images_registered.fetch_add(1, std::memory_order_relaxed);
    Image& image = slot_images[image_id];
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered),
               "Trying to register an already registered image");
    image.flags |= ImageFlagBits::Registered;
    WatchLog("register", image);
    ++registry_generation;
    total_used_memory += Common::AlignUp(image.info.guest_size, 1024);
    image.lru_id = lru_cache.Insert(image_id, gc_tick);
    image.lru_touched_tick = gc_tick;

    AliasState* state{};
    u32 aliases{};
    bool inserted{};
    ForEachAlias(image, [&](ImageId candidate_id, Image& candidate) {
        if (!state) {
            auto result = alias_states.try_emplace(image.info.guest_address);
            state = std::addressof(result.first.value());
            inserted = result.second;
        }
        ++aliases;
        candidate.flags |= ImageFlagBits::Aliased;
        if (!IsLiveImage(state->backing, state->backing_uid) ||
            slot_images[state->backing].alias_generation < candidate.alias_generation) {
            state->backing = candidate_id, state->backing_uid = candidate.image_uid;
        }
    });
    if (state) {
        image.flags |= ImageFlagBits::Aliased;
        state->members = inserted ? aliases + 1 : state->members + 1;
    }

    ForEachPage(image.info.guest_address, image.info.guest_size,
                [this, image_id](u64 page) { page_table[page].push_back(image_id); });
}

void TextureCache::UnregisterImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    WatchLog("unregister", image);
    ASSERT_MSG(True(image.flags & ImageFlagBits::Registered),
               "Trying to unregister an already unregistered image");
    image.flags &= ~ImageFlagBits::Registered;
    ++registry_generation;
    lru_cache.Free(image.lru_id);
    total_used_memory -= Common::AlignUp(image.info.guest_size, 1024);
    ForEachPage(image.info.guest_address, image.info.guest_size, [this, image_id](u64 page) {
        const auto page_it = page_table.find(page);
        ASSERT_MSG(page_it, "Unregistering unregistered page={:#x}", page << Traits::PAGE_BITS);
        auto& image_ids = *page_it;
        const auto vector_it = std::ranges::find(image_ids, image_id);
        ASSERT_MSG(vector_it != image_ids.end(), "Unregistering unregistered image in page={:#x}",
                   page << Traits::PAGE_BITS);
        image_ids.erase(vector_it);
    });
}

void TextureCache::TrackImage(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_begin = image.info.guest_address;
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (image_begin == image.track_addr && image_end == image.track_addr_end) {
        return;
    }

    if (!image.IsTracked()) {
        // Re-track the whole image
        image.track_addr = image_begin;
        image.track_addr_end = image_end;
        tracker.UpdatePageWatchers(image_begin, image.info.guest_size, PageOp::Track);
    } else {
        if (image_begin < image.track_addr) {
            TrackImageHead(image_id);
        }
        if (image.track_addr_end < image_end) {
            TrackImageTail(image_id);
        }
    }
}

void TextureCache::TrackImageHead(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_begin = image.info.guest_address;
    if (image_begin == image.track_addr) {
        return;
    }
    ASSERT(image.track_addr != 0 && image_begin < image.track_addr);
    const auto size = image.track_addr - image_begin;
    image.track_addr = image_begin;
    tracker.UpdatePageWatchers(image_begin, size, PageOp::Track);
}

void TextureCache::TrackImageTail(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (image_end == image.track_addr_end) {
        return;
    }
    ASSERT(image.track_addr_end != 0 && image.track_addr_end < image_end);
    const auto addr = image.track_addr_end;
    const auto size = image_end - image.track_addr_end;
    image.track_addr_end = image_end;
    tracker.UpdatePageWatchers(addr, size, PageOp::Track);
}

void TextureCache::UntrackImage(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!image.IsTracked()) {
        return;
    }
    const auto addr = image.track_addr;
    const auto size = image.track_addr_end - image.track_addr;
    image.track_addr = 0;
    image.track_addr_end = 0;
    if (size != 0) {
        tracker.UpdatePageWatchers(addr, size, PageOp::Untrack);
    }
}

void TextureCache::UntrackImageHead(ImageId image_id) {
    auto& image = slot_images[image_id];
    const auto image_begin = image.info.guest_address;
    if (!image.IsTracked() || image_begin < image.track_addr) {
        return;
    }
    const auto addr = tracker.GetNextPageAddr(image_begin);
    const auto size = addr - image_begin;
    image.track_addr = addr;
    if (image.track_addr == image.track_addr_end) {
        // This image spans only 2 pages and both are modified,
        // but the image itself was not directly affected.
        // Cehck its hash later.
        MarkAsMaybeDirty(image_id, image);
    }
    tracker.UpdatePageWatchers(image_begin, size, PageOp::Untrack);
}

void TextureCache::UntrackImageTail(ImageId image_id) {
    auto& image = slot_images[image_id];
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (!image.IsTracked() || image.track_addr_end < image_end) {
        return;
    }
    ASSERT(image.track_addr_end != 0);
    const auto addr = tracker.GetPageAddr(image_end);
    const auto size = image_end - addr;
    image.track_addr_end = addr;
    if (image.track_addr == image.track_addr_end) {
        // This image spans only 2 pages and both are modified,
        // but the image itself was not directly affected.
        // Cehck its hash later.
        MarkAsMaybeDirty(image_id, image);
    }
    tracker.UpdatePageWatchers(addr, size, PageOp::Untrack);
}

void TextureCache::GarbageCollectImages() {
    Gow3Stats::Timer gc_timer{Gow3Stats::t_gc};
    if (instance.CanReportMemoryUsage()) {
        total_used_memory = instance.GetDeviceMemoryUsage();
        // gow3: on integrated GPUs (Steam Deck) the usage covers system-memory heaps holding
        // much more than images (buffers backing guest memory), and the startup budget left
        // ~1 GB after its 8 GB system reserve: usage stayed above the critical mark, so the
        // collector evicted images used two or three frames ago on every submission and wrote
        // GPU-written ones back. Compare with the driver's current budget instead.
        // GOW3_GC_BUDGET_MB=N: this rule with a fixed budget on any GPU (tests on a desktop).
        static const u64 forced_budget = [] {
            const char* env = std::getenv("GOW3_GC_BUDGET_MB");
            return env ? std::strtoull(env, nullptr, 10) << 20 : 0;
        }();
        // GOW3_GC_LIVE_BUDGET=1: the same rule on dedicated GPUs. At startup the limits keep 60% of
        // the budget free on cards up to 8 GB: a friend's RTX 3070 (6.3 GB budget at 1440p in UC2)
        // evicted images from 2.5 GB on, all session long; with this it starts at 85% (5.4 GB).
        static const bool live_budget = [] {
            const char* env = std::getenv("GOW3_GC_LIVE_BUDGET");
            return env && env[0] == '1';
        }();
        if (instance.IsIntegrated() || forced_budget || live_budget) {
            const u64 budget = forced_budget ? forced_budget : instance.GetDeviceMemoryBudgetNow();
            if (budget != 0) {
                trigger_gc_memory = budget / 10 * 7;
                pressure_gc_memory = budget / 100 * 85;
                critical_gc_memory = budget / 100 * 95;
            }
        }
    }
    if (total_used_memory < trigger_gc_memory) {
        return;
    }
    std::scoped_lock lock{mutex};
    bool pressured = false;
    bool aggresive = false;
    u64 ticks_to_destroy = 0;
    size_t num_deletions = 0;

    const auto configure = [&](bool allow_aggressive) {
        pressured = total_used_memory >= pressure_gc_memory;
        aggresive = allow_aggressive && total_used_memory >= critical_gc_memory;
        ticks_to_destroy = aggresive ? 160 : pressured ? 80 : 16;
        ticks_to_destroy = std::min(ticks_to_destroy, gc_tick);
        num_deletions = aggresive ? 40 : pressured ? 20 : 10;
    };
    const auto clean_up = [&](ImageId image_id) {
        if (num_deletions == 0) {
            return true;
        }
        --num_deletions;
        auto& image = slot_images[image_id];
        ResolveReadbacks(image.info.guest_address, image.info.guest_size, true,
                         ReadbackSource::CacheCleanup);
        const bool download = image.SafeToDownload();
        const bool tiled = image.info.IsTiled();
        if (tiled && download) {
            // This is a workaround for now. We can't handle non-linear image downloads.
            return false;
        }
        if (download && !pressured) {
            return false;
        }
        if (download) {
            // gow3: synchronously, while the image still protects its pages. A deferred
            // write-back landed after FreeImage had unprotected them, over whatever the game
            // had meanwhile stored there (e.g. its heap after unloading an area).
            DownloadImageMemory(image_id, true);
            ++gc_downloads;
        }
        ++gc_evictions;
        FreeImage(image_id);
        if (total_used_memory < critical_gc_memory) {
            if (aggresive) {
                num_deletions >>= 2;
                aggresive = false;
                return false;
            }
            if (pressured && total_used_memory < pressure_gc_memory) {
                num_deletions >>= 1;
                pressured = false;
            }
        }
        return false;
    };

    // Try to remove anything old enough and not high priority.
    configure(false);
    lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);

    if (total_used_memory >= critical_gc_memory) {
        // If we are still over the critical limit, run an aggressive GC
        configure(true);
        lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);
    }
    // gow3: evictions under memory pressure, at most every 5 s (GOW3_FRAME_STATS or not).
    if (pressured || gc_downloads != 0) {
        const auto now = std::chrono::steady_clock::now();
        if (now - gc_report_time >= std::chrono::seconds(5)) {
            std::printf("Texture cache: memory pressure, %llu of %llu MiB (critical %llu): "
                        "%llu images evicted, %llu written back since the last report\n",
                        (unsigned long long)(total_used_memory >> 20),
                        (unsigned long long)(pressure_gc_memory >> 20),
                        (unsigned long long)(critical_gc_memory >> 20),
                        (unsigned long long)gc_evictions, (unsigned long long)gc_downloads);
            gc_report_time = now;
            gc_evictions = gc_downloads = 0;
        }
    }
}

void TextureCache::GarbageCollectSamplers() {
    total_used_samplers = samplers.size();
    if (total_used_samplers < trigger_gc_samplers) {
        return;
    }
    std::scoped_lock lock{samplers_mutex};
    bool pressured = false;
    bool aggresive = false;
    u64 ticks_to_destroy = 0;
    size_t num_deletions = 0;

    const auto configure = [&](bool allow_aggressive) {
        pressured = total_used_samplers >= pressure_gc_samplers;
        aggresive = allow_aggressive && total_used_samplers >= critical_gc_samplers;
        ticks_to_destroy = aggresive ? 160 : pressured ? 80 : 16;
        ticks_to_destroy = std::min(ticks_to_destroy, gc_tick);
        num_deletions = aggresive ? 40 : pressured ? 20 : 10;
    };
    const auto clean_up = [&](u64 hash) {
        if (num_deletions == 0) {
            return true;
        }
        --num_deletions;
        const size_t lru_id = samplers.at(hash).lru_id;
        samplers.erase(hash);
        sampler_lru_cache.Free(lru_id);
        return false;
    };

    // Try to remove anything old enough and not high priority.
    configure(false);
    sampler_lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);

    if (total_used_samplers >= critical_gc_samplers) {
        // If we are still over the critical limit, run an aggressive GC
        configure(true);
        sampler_lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);
    }
}

void TextureCache::RunGarbageCollector() {
    SCOPE_EXIT {
        ++gc_tick;
    };

    // uc3: GOW3_RELOAD_AT=<s> - diagnostic: once, s seconds after start, re-upload every image
    // the GPU never wrote from guest memory. If a wrong texture turns right, the cache held stale
    // content; if not, guest memory itself (or a buffer) is wrong.
    static const double reload_at = [] {
        const char* env = std::getenv("GOW3_RELOAD_AT");
        return env ? std::atof(env) : -1.0;
    }();
    static bool reloaded = false;
    // uc1 (GOW3_RELOAD_FILE=<path>, bright crates): the same re-upload, each time that file
    // appears (a .bat creates it while the bad texture is on screen); the file is consumed.
    static const char* reload_file = std::getenv("GOW3_RELOAD_FILE");
    const bool reload_now = reload_file && *reload_file && std::remove(reload_file) == 0;
    if (reload_now || (reload_at >= 0 && !reloaded &&
        std::chrono::duration<double>(std::chrono::steady_clock::now() - uc3_flash_clock).count() >=
            reload_at)) {
        reloaded = true;
        std::scoped_lock lk{mutex};
        u32 count = 0;
        lru_cache.ForEachItemBelow(gc_tick, [&](ImageId id) {
            Image& image = slot_images[id];
            if (True(image.flags & ImageFlagBits::Registered) &&
                False(image.flags & ImageFlagBits::GpuModified)) {
                image.flags |= ImageFlagBits::CpuDirty;
                UntrackImage(id);
                ++count;
            }
        });
        std::printf("uc3: GOW3_RELOAD_AT/FILE: %u images marked for re-upload\n", count);
        std::fflush(stdout);
    }

    GarbageCollectImages();
    GarbageCollectSamplers();
}

void TextureCache::TouchImage(const Image& image) {
    if (image.lru_touched_tick == gc_tick) {
        return;
    }
    image.lru_touched_tick = gc_tick;
    lru_cache.Touch(image.lru_id, gc_tick);
}

void TextureCache::DeleteImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    WatchLog("delete", image);
    CancelReadbacks(image.info.guest_address, image.info.guest_size);
    ASSERT_MSG(!image.IsTracked(), "Image was not untracked");
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered), "Image was not unregistered");

    const auto alias_it = True(image.flags & ImageFlagBits::Aliased)
                              ? alias_states.find(image.info.guest_address)
                              : alias_states.end();
    if (alias_it != alias_states.end()) {
        AliasState& state = alias_it.value();
        // ponytail: no copy here (DeleteImage also runs on guest threads through UnmapMemory,
        // which must not record commands); the aliases then re-read guest memory.
        if ((state.writer == image_id && state.writer_uid == image.image_uid) ||
            (state.backing == image_id && state.backing_uid == image.image_uid)) {
            state.ResetAuthority();
        }
        if (state.members <= 2) {
            ForEachAlias(image, [](ImageId, Image& candidate) {
                candidate.flags &= ~ImageFlagBits::Aliased;
            });
            alias_states.erase(alias_it);
        } else {
            --state.members;
        }
    }

    // Remove any registered meta areas.
    const auto& meta_info = image.info.meta_info;
    if (meta_info.cmask_addr) {
        UnregisterMeta(meta_info.cmask_addr, image.image_uid);
    }
    if (meta_info.fmask_addr) {
        UnregisterMeta(meta_info.fmask_addr, image.image_uid);
    }
    if (meta_info.htile_addr) {
        UnregisterMeta(meta_info.htile_addr, image.image_uid);
    }

    {
        std::unique_lock lk{download_images_mutex};
        if (download_images.contains(image_id)) {
            download_images.erase(image_id);
        }
        tiny_download_images.erase(image_id);
    }

    // Reclaim image and any image views it references.
    scheduler.DeferOperation([this, image_id] {
        Image& image = slot_images[image_id];
        for (auto& backing : image.backing_images) {
            for (const ImageViewId image_view_id : backing.image_view_ids) {
                slot_image_views.Erase(image_view_id);
            }
        }
        slot_images.Erase(image_id);
    });
}

} // namespace VideoCore
