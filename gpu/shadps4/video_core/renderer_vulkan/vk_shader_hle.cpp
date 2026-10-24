// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "core/memory.h"
#include "shader_recompiler/info.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/host_shaders/buffer_multi_copy_comp.h"
#include "gow3_toggles.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"

extern std::unique_ptr<AmdGpu::Liverpool> liverpool;

namespace Vulkan {

static constexpr u64 COPY_SHADER_HASH = 0xfefebf9f;

// gow3: the copy shader runs ~57 times per frame with ~1024 small ranges each. As one
// vkCmdCopyBuffer with that many regions it cost ~37 ns of GPU time per range (~2.1 ms per
// frame) plus the recording; buffer_multi_copy.comp copies all ranges in one dispatch.
static bool MultiCopy(Rasterizer& rasterizer, const VideoCore::Buffer* src,
                      const VideoCore::Buffer* dst, std::span<const vk::BufferCopy> copies) {
    if (copies.size() < 8 || Gow3Toggle::Disabled(Gow3Toggle::MultiCopyShader)) {
        return false;
    }
    u64 src_min = ~0ull, src_max = 0, dst_min = ~0ull, dst_max = 0;
    for (const auto& copy : copies) {
        if ((copy.srcOffset | copy.dstOffset | copy.size) & 3) {
            return false; // dword copies only
        }
        src_min = std::min(src_min, copy.srcOffset);
        src_max = std::max(src_max, copy.srcOffset + copy.size);
        dst_min = std::min(dst_min, copy.dstOffset);
        dst_max = std::max(dst_max, copy.dstOffset + copy.size);
    }
    auto& runtime = rasterizer.GetRuntime();
    auto& scheduler = runtime.GetScheduler();
    const auto& instance = runtime.GetInstance();
    const u64 align = instance.StorageMinAlignment();
    src_min = Common::AlignDown(src_min, align);
    dst_min = Common::AlignDown(dst_min, align);

    struct Pipeline {
        vk::UniqueDescriptorSetLayout set_layout;
        vk::UniquePipelineLayout layout;
        vk::UniquePipeline pipeline;
    };
    static Pipeline pipe = [&] {
        const auto device = instance.GetDevice();
        Pipeline p;
        std::array<vk::DescriptorSetLayoutBinding, 3> bindings{};
        for (u32 i = 0; i < 3; ++i) {
            bindings[i] = {.binding = i,
                           .descriptorType = vk::DescriptorType::eStorageBuffer,
                           .descriptorCount = 1,
                           .stageFlags = vk::ShaderStageFlagBits::eCompute};
        }
        p.set_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = static_cast<u32>(bindings.size()),
            .pBindings = bindings.data(),
        }));
        const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                          .offset = 0,
                                          .size = sizeof(u32)};
        p.layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &*p.set_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &range,
        }));
        const auto module = CompileSPV(BUFFER_MULTI_COPY_COMP, device);
        p.pipeline = Check(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = module,
                              .pName = "main"},
                    .layout = *p.layout,
                }));
        device.destroyShaderModule(module);
        return p;
    }();

    // The region table goes into the stream buffer (host writes, visible at submission).
    thread_local std::vector<u32> regions;
    regions.resize(copies.size() * 4);
    for (size_t i = 0; i < copies.size(); ++i) {
        regions[i * 4 + 0] = static_cast<u32>((copies[i].srcOffset - src_min) / 4);
        regions[i * 4 + 1] = static_cast<u32>((copies[i].dstOffset - dst_min) / 4);
        regions[i * 4 + 2] = static_cast<u32>(copies[i].size / 4);
        regions[i * 4 + 3] = 0;
    }
    auto& stream = rasterizer.GetBufferCache().GetStreamBuffer();
    const u64 table_size = regions.size() * sizeof(u32);
    const u64 table_offset = stream.Copy(regions.data(), table_size, align);

    if (auto* profiler = GpuProfiler::Get()) {
        profiler->Mark(0xC0B1ull, [] { return std::string{"copy shader HLE: barrier + dispatch"}; });
    }
    scheduler.EndRendering();
    const u64 src_size = src_max - src_min, dst_size = dst_max - dst_min;
    if (runtime.IsBufferAccessed(src, src_min, src_size) ||
        runtime.IsBufferAccessed(dst, dst_min, dst_size, true)) {
        runtime.FlushBarriers();
    }
    const u32 count = static_cast<u32>(copies.size());
    scheduler.Record([src = src->Handle(), dst = dst->Handle(), table = stream.Handle(), src_min,
                      src_size, dst_min, dst_size, table_offset, table_size,
                      count](vk::CommandBuffer cmdbuf) {
        const std::array<vk::DescriptorBufferInfo, 3> infos{{
            {src, src_min, src_size},
            {dst, dst_min, dst_size},
            {table, table_offset, table_size},
        }};
        std::array<vk::WriteDescriptorSet, 3> writes{};
        for (u32 i = 0; i < 3; ++i) {
            writes[i] = {.dstBinding = i,
                         .descriptorCount = 1,
                         .descriptorType = vk::DescriptorType::eStorageBuffer,
                         .pBufferInfo = &infos[i]};
        }
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *pipe.pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pipe.layout, 0, writes);
        cmdbuf.pushConstants(*pipe.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(count),
                             &count);
        cmdbuf.dispatch(count, 1, 1);
    });
    runtime.AccessBuffer(src, src_min, src_size, vk::PipelineStageFlagBits2::eComputeShader,
                         vk::AccessFlagBits2::eShaderRead);
    runtime.AccessBuffer(dst, dst_min, dst_size, vk::PipelineStageFlagBits2::eComputeShader,
                         vk::AccessFlagBits2::eShaderWrite);
    return true;
}

static bool ExecuteCopyShaderHLE(const Shader::Info& info, const AmdGpu::ComputeProgram& cs_program,
                                 Rasterizer& rasterizer) {
    auto& runtime = rasterizer.GetRuntime();
    auto& buffer_cache = rasterizer.GetBufferCache();

    // Copy shader defines three formatted buffers as inputs: control, source, and destination.
    const auto ctl_buf_sharp = info.buffers[0].GetSharp(info);
    const auto src_buf_sharp = info.buffers[1].GetSharp(info);
    const auto dst_buf_sharp = info.buffers[2].GetSharp(info);
    const auto buf_stride = src_buf_sharp.GetStride();
    ASSERT(buf_stride == dst_buf_sharp.GetStride());

    struct CopyShaderControl {
        u32 dst_idx;
        u32 src_idx;
        u32 end;
    };
    static_assert(sizeof(CopyShaderControl) == 12);
    ASSERT(ctl_buf_sharp.GetStride() == sizeof(CopyShaderControl));
    const auto ctl_buf = reinterpret_cast<const CopyShaderControl*>(ctl_buf_sharp.base_address);

    static std::vector<vk::BufferCopy> copies;
    copies.clear();
    copies.reserve(cs_program.dim_x);

    for (u32 i = 0; i < cs_program.dim_x; i++) {
        const auto& [dst_idx, src_idx, end] = ctl_buf[i];
        const u32 local_dst_offset = dst_idx * buf_stride;
        const u32 local_src_offset = src_idx * buf_stride;
        const u32 local_size = (end + 1) * buf_stride;
        copies.emplace_back(local_src_offset, local_dst_offset, local_size);
    }

    // gow3: 64 KiB instead of 64 MiB. The copies are a few KiB spread over up to 57 MiB, and
    // each batch synchronizes (uploads, marks GPU-modified) its whole range: GPU time of the copy
    // shader 1.5 -> 0.6 ms/frame, GPU busy 85% -> 77%, frame rate no lower.
    // GOW3_COPY_MERGE_KB overrides it.
    static const vk::DeviceSize MaxDistanceForMerge = [] {
        const char* env = std::getenv("GOW3_COPY_MERGE_KB");
        return env ? vk::DeviceSize(std::strtoull(env, nullptr, 10)) * 1024 : vk::DeviceSize(64_KB);
    }();
    u32 batch_start = 0;
    u32 batch_end = 0;

    while (batch_end < copies.size()) {
        // Place first copy into the current batch
        const auto& copy = copies[batch_start];
        auto src_offset_min = copy.srcOffset;
        auto src_offset_max = copy.srcOffset + copy.size;
        auto dst_offset_min = copy.dstOffset;
        auto dst_offset_max = copy.dstOffset + copy.size;

        for (++batch_end; batch_end < copies.size(); batch_end++) {
            // Compute new src and dst bounds if we were to batch this copy
            const auto& [src_offset, dst_offset, size] = copies[batch_end];
            auto new_src_offset_min = std::min(src_offset_min, src_offset);
            auto new_src_offset_max = std::max(src_offset_max, src_offset + size);
            if (new_src_offset_max - new_src_offset_min > MaxDistanceForMerge) {
                break;
            }

            auto new_dst_offset_min = std::min(dst_offset_min, dst_offset);
            auto new_dst_offset_max = std::max(dst_offset_max, dst_offset + size);
            if (new_dst_offset_max - new_dst_offset_min > MaxDistanceForMerge) {
                break;
            }

            // We can batch this copy
            src_offset_min = new_src_offset_min;
            src_offset_max = new_src_offset_max;
            dst_offset_min = new_dst_offset_min;
            dst_offset_max = new_dst_offset_max;
        }

        // Obtain buffers for the total source and destination ranges.
        const auto [src_buf, src_buf_offset] = buffer_cache.ObtainBuffer(
            src_buf_sharp.base_address + src_offset_min, src_offset_max - src_offset_min, false);
        const auto [dst_buf, dst_buf_offset] = buffer_cache.ObtainBuffer(
            dst_buf_sharp.base_address + dst_offset_min, dst_offset_max - dst_offset_min, true);

        // Apply found buffer base.
        const auto vk_copies = std::span{copies}.subspan(batch_start, batch_end - batch_start);
        for (auto& copy : vk_copies) {
            copy.srcOffset = copy.srcOffset - src_offset_min + src_buf_offset;
            copy.dstOffset = copy.dstOffset - dst_offset_min + dst_buf_offset;
        }

        // Execute buffer copies.
        LOG_TRACE(Render_Vulkan, "HLE buffer copy: src_size = {}, dst_size = {}",
                  src_offset_max - src_offset_min, dst_offset_max - dst_offset_min);
        if (!MultiCopy(rasterizer, src_buf, dst_buf, vk_copies)) {
            runtime.CopyBuffer(src_buf, dst_buf, vk_copies);
        }
        batch_start = batch_end;
    }

    return true;
}

// uc3 (from SuperMedo's uncharted_pc, GPL: ExecuteScalarCopyHLE in vk_shader_hle.cpp):
// Uncharted fills small constant blocks (effect material parameters, character constants) with a
// compute shader that copies one dword per workgroup: dst[i] = src[i] while i < count (user data
// SGPR6). Run on the GPU, the copy never reaches guest memory, and the SRT walker reads those
// blocks there: the streets' remaining synchronous readbacks (resultados-readback-opus.md, R.7).
// GOW3_SCALAR_COPY_HLE=1 (off by default): when the GPU wrote neither side, the copy is made in
// guest memory and the dispatch is skipped; next to GPU data (pages the GPU wrote) the GPU copy
// still runs and small copies whose source bytes the GPU did not write are also made in guest
// memory. GOW3_DEPTH_COPY_SYNC=0: no image write-back before copies of 1 MiB or more (depth
// surfaces the depth of field reads).
static constexpr u64 SCALAR_COPY_SHADER_HASH = 0x2d215c3b;   // one dword per workgroup
static constexpr u64 SCALAR_COPY64_SHADER_HASH = 0x6a6422a9; // 64 dwords per workgroup (U2)

extern "C" int runtime_memory_region_backing(uintptr_t address, uintptr_t* start, uintptr_t* end,
                                             int* mapped, const unsigned char** backing);

static bool ScalarCopyHLEEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_SCALAR_COPY_HLE");
        return env && env[0] == '1';
    }();
    return enabled;
}

// The source through the backing view: its page may be read-protected for GPU data next to it,
// and a fault here would wait for the GPU. Uploads of guest memory for the commands before this
// one run later, on the recording and copy threads: they finish first, or a draw recorded before
// the copy would upload the destination with the copy's result (wrong material constants:
// alpha-tested doors and hair drawn with another object's parameters).
// GOW3_SCALAR_COPY_NOWAIT=1: no wait (A/B).
static bool CopyGuestThroughBacking(Rasterizer& rasterizer, VAddr dst, VAddr src, u64 size) {
    static const bool no_wait = [] {
        const char* env = std::getenv("GOW3_SCALAR_COPY_NOWAIT");
        return env && env[0] == '1';
    }();
    // R.8.5: with GOW3_HLE_PRECISE_WAIT=1, wait only while a deferred upload of the destination
    // is pending (it would read the new bytes); the source is not changed, so its reads are fine.
    static std::atomic<u32> precise_skips{0}, precise_waits{0};
    if (!no_wait) {
        if (VideoCore::BufferCache::PreciseHostReadWait() &&
            !VideoCore::BufferCache::HasPendingHostReads(dst, size)) {
            const u32 n = precise_skips.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n == 1 || (n & 65535) == 0) {
                std::printf("uc3: scalar copy HLE precise wait: %u skipped, %u waits\n", n,
                            precise_waits.load());
            }
        } else {
            if (VideoCore::BufferCache::PreciseHostReadWait()) {
                const u32 n = precise_waits.fetch_add(1, std::memory_order_relaxed) + 1;
                if (n <= 3 || (n & 1023) == 0) {
                    std::printf("uc3: scalar copy HLE waited for host copies of 0x%llx (%u waits, "
                                "%u skipped)\n",
                                static_cast<unsigned long long>(dst), n, precise_skips.load());
                }
            }
            rasterizer.WaitHostCopies();
        }
    }
    uintptr_t start = 0, end = 0;
    int mapped = 0;
    const unsigned char* backing = nullptr;
    if (!runtime_memory_region_backing(src, &start, &end, &mapped, &backing) || !mapped ||
        !backing || end - src < size) {
        return false;
    }
    // uc3 (GOW3_HLE_INVALIDATE=1): the backing write bypasses page protection, so the caches
    // never learn of it and later draws may use their stale GPU copy (swapped textures). Do
    // what a CPU write fault does, before the write (after it, the invalidation would download
    // the old GPU bytes over the copy): constant ring copies queued so far, GPU data of the
    // source not read back yet, then the destination marked CPU-modified.
    static const bool invalidate = [] {
        const char* env = std::getenv("GOW3_HLE_INVALIDATE");
        return env && env[0] == '1';
    }();
    if (invalidate) {
        static std::atomic<u32> calls{0}, src_readbacks{0};
        rasterizer.WaitRingCopies();
        auto& texture_cache = rasterizer.GetTextureCache();
        if (texture_cache.HasPendingReadbackBytes(src, size)) {
            src_readbacks.fetch_add(1, std::memory_order_relaxed);
            texture_cache.ResolveReadbacks(src, size, true, VideoCore::ReadbackSource::CpuRead);
        }
        rasterizer.InvalidateMemory(dst, size, true);
        const u32 n = calls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n == 1 || (n & 16383) == 0) {
            std::printf("uc3: scalar copy HLE invalidate: %u copies, %u source readbacks\n", n,
                        src_readbacks.load());
        }
    }
    rasterizer.NoteGuestWrite(dst, size);
    const bool written =
        Core::Memory::Instance()->TryWriteBacking(reinterpret_cast<void*>(dst), backing, size);
    SrtNoteWrite(dst, size, SrtOriginRewrite); // uc3 R.21: again once written
    return written;
}

static bool ExecuteScalarCopyHLE(const Shader::Info& info, const AmdGpu::ComputeProgram& cs_program,
                                 Rasterizer& rasterizer, u64 group_size) {
    static std::atomic<u32> rejects{0};
    const auto reject = [&](const char* why) {
        if (rejects.fetch_add(1, std::memory_order_relaxed) < 8) {
            std::printf("uc3: scalar copy HLE %llx refused: %s (buffers %zu, user data %zu)\n",
                        static_cast<unsigned long long>(info.pgm_hash), why, info.buffers.size(),
                        info.UserData().size());
        }
        return false;
    };
    // Our recompiler also lists its own buffers (the flattened user data): the guest's two only.
    const Shader::BufferResource* guest[2]{};
    u32 guest_count = 0;
    for (const auto& buffer : info.buffers) {
        if (!buffer.IsSpecial()) {
            if (guest_count < 2) {
                guest[guest_count] = &buffer;
            }
            ++guest_count;
        }
    }
    if (guest_count != 2) {
        return reject("buffer count");
    }
    const auto ud = info.UserData();
    if (ud.size() <= 6) {
        return reject("user data size");
    }
    const auto src = guest[0]->GetSharp(info);
    const auto dst = guest[1]->GetSharp(info);
    if (src.GetStride() != 4 || dst.GetStride() != 4 || !src.base_address || !dst.base_address) {
        std::printf("uc3: scalar copy HLE strides %u/%u, bases 0x%llx/0x%llx\n", src.GetStride(),
                    dst.GetStride(), static_cast<unsigned long long>(src.base_address),
                    static_cast<unsigned long long>(dst.base_address));
        return reject("stride or base");
    }
    const u64 count = std::min<u64>(
        {u64(cs_program.dim_x) * group_size, ud[6], src.num_records, dst.num_records});
    if (count == 0) {
        return true;
    }
    const u64 size = count * 4;
    auto* memory = Core::Memory::Instance();
    if (!memory->IsValidGpuMapping(src.base_address, size) ||
        !memory->IsValidGpuMapping(dst.base_address, size)) {
        return reject("mapping");
    }
    auto& buffer_cache = rasterizer.GetBufferCache();
    // uc3 (GOW3_HLE_READBACK_WAIT=1): an async buffer readback clears the source's GPU marks when
    // it starts, before its bytes land in guest memory. Copying in between takes stale data and
    // skips the dispatch (UC2: the train carriage fell early with READBACK_ASYNC + this HLE).
    // Let overlapping readbacks land first; a source still GPU-written keeps the dispatch.
    static const bool readback_wait = [] {
        const char* env = std::getenv("GOW3_HLE_READBACK_WAIT");
        return env && env[0] == '1';
    }();
    if (readback_wait) {
        // The destination too: a readback landing after the copy would overwrite it.
        buffer_cache.WaitReadbacksLanded(src.base_address, size);
        buffer_cache.WaitReadbacksLanded(dst.base_address, size);
    }
    // uc3 (GOW3_HLE_COMPUTE_SAFE): the UC2 train carriage still fell early with this HLE,
    // GOW3_COMPUTE_PIPE and the async readbacks all on (any two of them pass).
    // 1: a write handed to the recording thread and not recorded yet overlaps the source or the
    // destination: the dispatch runs. 2: also every copy issued from an async compute queue.
    static const int compute_safe = [] {
        const char* env = std::getenv("GOW3_HLE_COMPUTE_SAFE");
        return env ? std::atoi(env) : 0;
    }();
    if (compute_safe) {
        static std::atomic<u32> pending_hits{0}, queue_hits{0};
        const bool pending = rasterizer.HlePendingWrite(src.base_address, size) ||
                             rasterizer.HlePendingWrite(dst.base_address, size);
        if (pending || (compute_safe >= 2 && rasterizer.OnComputeQueue())) {
            const u32 n = (pending ? pending_hits : queue_hits).fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 3 || (n & 4095) == 0) {
                std::printf("uc3: scalar copy HLE compute safe: %u pending-write fallbacks, %u "
                            "async compute queue fallbacks\n",
                            pending_hits.load(), queue_hits.load());
            }
            return false;
        }
    }
    static const bool depth_sync = [] {
        const char* env = std::getenv("GOW3_DEPTH_COPY_SYNC");
        return !(env && env[0] == '0');
    }();
    if (depth_sync && size >= 1_MB) {
        // The scene depth is copied this way; its image lives on the GPU only, so its contents go
        // into the source buffer first (SuperMedo: without it the depth of field blurred all).
        buffer_cache.SyncImageToBuffer(src.base_address, static_cast<u32>(size));
    }
    static std::atomic<u32> guest_only{0}, both{0}, gpu_only{0};
    const auto report = [&](std::atomic<u32>& counter, const char* what) {
        const u32 n = counter.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 3 || (n & 4095) == 0) {
            std::printf("uc3: scalar copy HLE %llx: 0x%llx -> 0x%llx (%llu bytes): %s (%u; guest "
                        "%u, both %u, GPU %u)\n",
                        static_cast<unsigned long long>(info.pgm_hash),
                        static_cast<unsigned long long>(src.base_address),
                        static_cast<unsigned long long>(dst.base_address),
                        static_cast<unsigned long long>(size), what, n, guest_only.load(),
                        both.load(), gpu_only.load());
        }
    };
    const bool src_gpu = buffer_cache.IsRegionGpuModified(src.base_address, size);
    const bool dst_gpu = buffer_cache.IsRegionGpuModified(dst.base_address, size);
    if (src_gpu || dst_gpu) {
        // The GPU copy keeps the GPU's view right; the page tracker flags whole pages, so most
        // of these sources hold no GPU bytes and their guest memory is current.
        if (size < 1_MB && !buffer_cache.IsGpuWritten(src.base_address, size) &&
            CopyGuestThroughBacking(rasterizer, dst.base_address, src.base_address, size)) {
            report(both, "GPU copy and guest copy");
        } else {
            report(gpu_only, "GPU copy only");
        }
        return false;
    }
    if (readback_wait && buffer_cache.IsGpuWritten(src.base_address, size)) {
        report(gpu_only, "GPU copy only (source GPU-written)");
        return false;
    }
    if (!CopyGuestThroughBacking(rasterizer, dst.base_address, src.base_address, size)) {
        return reject("backing copy");
    }
    // The GPU's copy of the destination is refreshed from guest memory when next used.
    buffer_cache.InvalidateMemory(dst.base_address, size, true);
    report(guest_only, "guest copy, dispatch skipped");
    return true;
}

// uc3 (GOW3_HLE_INORDER=1, off by default; Opus's design): a scalar copy from an asynchronous
// compute queue without draining the draw pipe first. Stage A takes the source bytes now (only
// when no queued write overlaps them) and hands the destination write to the recording thread,
// in order with the draws; until it lands, stage A treats the destination as a pending GPU write
// (constants bound there, flattened leaves copied there). UC2 rooftop: ~55k drains/s, nearly all
// before these copies.
namespace {
struct InOrderCopy {
    u64 dst;
    u32 size;
    u32 pad;
};
constexpr u32 IN_ORDER_COPY_MAX = 256;
// uc3 A4.1: GOW3_HLE_INORDER_MAX=<bytes> raises the size limit (default 256, at most 64 KiB);
// in the Cidadela most fallbacks were copies above 256 bytes.
u32 InOrderCopyMax() {
    static const u32 max = [] {
        const char* env = std::getenv("GOW3_HLE_INORDER_MAX");
        const long v = env ? std::strtol(env, nullptr, 10) : 0;
        return v > 0 ? static_cast<u32>(std::min<long>(v, 65536)) : IN_ORDER_COPY_MAX;
    }();
    return max;
}

void RunInOrderCopy(Rasterizer& rasterizer, const u8* data) {
    const auto& copy = *reinterpret_cast<const InOrderCopy*>(data);
    const u8* bytes = data + sizeof(InOrderCopy);
    // As RunWriteData: copies deferred to the recording thread precede the write; the write
    // itself goes through the page tracker (a protected page faults and invalidates).
    rasterizer.WaitHostCopies();
    std::memcpy(reinterpret_cast<void*>(copy.dst), bytes, copy.size);
    SrtNoteWrite(copy.dst, copy.size, SrtOriginRewrite);
}
[[maybe_unused]] const bool task_name_registered = [] {
    {
        Gow3Stats::NameTask(reinterpret_cast<const void*>(&RunInOrderCopy), "in-order copy");
    }
    return true;
}();
} // namespace

bool HleInOrderEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_HLE_INORDER");
        return env && env[0] == '1';
    }();
    return enabled;
}

// uc3: why an in-order copy fell back to the drain.
static std::atomic<u32> in_order_rejects[9];
void NoteInOrderReject(unsigned reason) {
    const u32 n = in_order_rejects[reason].fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1 || (n & 65535) == 0) {
        std::printf("uc3: in-order copy fallbacks: hash %u, sharps %u, size %u, mapping %u, "
                    "pending %u, gpu %u, backing %u, not HLE %u, no pipeline %u\n",
                    in_order_rejects[0].load(), in_order_rejects[1].load(),
                    in_order_rejects[2].load(), in_order_rejects[3].load(),
                    in_order_rejects[4].load(), in_order_rejects[5].load(),
                    in_order_rejects[6].load(), in_order_rejects[7].load(),
                    in_order_rejects[8].load());
    }
}

bool ExecuteScalarCopyInOrder(const Shader::Info& info, const AmdGpu::ComputeProgram& cs_program,
                              Rasterizer& rasterizer) {
    if (!ScalarCopyHLEEnabled()) {
        return false;
    }
    const auto reject = [](u32 reason) {
        NoteInOrderReject(reason);
        return false;
    };
    u64 group_size = 0;
    if (info.pgm_hash == SCALAR_COPY_SHADER_HASH) {
        group_size = 1;
    } else if (info.pgm_hash == SCALAR_COPY64_SHADER_HASH) {
        group_size = 64;
    } else {
        return reject(0);
    }
    const Shader::BufferResource* guest[2]{};
    u32 guest_count = 0;
    for (const auto& buffer : info.buffers) {
        if (!buffer.IsSpecial()) {
            if (guest_count < 2) {
                guest[guest_count] = &buffer;
            }
            ++guest_count;
        }
    }
    const auto ud = info.UserData();
    if (guest_count != 2 || ud.size() <= 6) {
        return reject(1);
    }
    const auto src = guest[0]->GetSharp(info);
    const auto dst = guest[1]->GetSharp(info);
    if (src.GetStride() != 4 || dst.GetStride() != 4 || !src.base_address || !dst.base_address) {
        return reject(1);
    }
    const u64 count = std::min<u64>(
        {u64(cs_program.dim_x) * group_size, ud[6], src.num_records, dst.num_records});
    const u64 size = count * 4;
    if (count == 0 || size > InOrderCopyMax()) {
        static std::atomic<u64> biggest{0};
        if (size > biggest.load(std::memory_order_relaxed)) {
            biggest.store(size, std::memory_order_relaxed);
            std::printf("uc3: in-order copy too big: %llu bytes (limit %u)\n",
                        static_cast<unsigned long long>(size), InOrderCopyMax());
        }
        Gow3Stats::size_reject_hist[Gow3Stats::SizeBucket(size)].fetch_add(
            1, std::memory_order_relaxed);
        return reject(2);
    }
    auto* memory = Core::Memory::Instance();
    if (!memory->IsValidGpuMapping(src.base_address, size) ||
        !memory->IsValidGpuMapping(dst.base_address, size)) {
        return reject(3);
    }
    // Queued writes (draws, DMA, earlier in-order copies) over either side, or GPU data there:
    // the draining path decides.
    if (rasterizer.HlePendingWrite(src.base_address, size) ||
        rasterizer.HlePendingWrite(dst.base_address, size)) {
        return reject(4);
    }
    auto& buffer_cache = rasterizer.GetBufferCache();
    if (buffer_cache.IsRegionGpuModified(src.base_address, size) ||
        buffer_cache.IsRegionGpuModified(dst.base_address, size) ||
        rasterizer.IsGpuWritten(src.base_address, size)) {
        return reject(5);
    }
    uintptr_t start = 0, end = 0;
    int mapped = 0;
    const unsigned char* backing = nullptr;
    if (!runtime_memory_region_backing(src.base_address, &start, &end, &mapped, &backing) ||
        !mapped || !backing || end - src.base_address < size) {
        return reject(6);
    }
    thread_local std::vector<u8> packet_buffer;
    packet_buffer.resize(sizeof(InOrderCopy) + size);
    u8* packet = packet_buffer.data();
    *reinterpret_cast<InOrderCopy*>(packet) = {dst.base_address, static_cast<u32>(size), 0};
    std::memcpy(packet + sizeof(InOrderCopy), backing, size);
    if (rasterizer.RunInOrder(&RunInOrderCopy, packet, static_cast<u32>(sizeof(InOrderCopy) + size))) {
        rasterizer.NotePendingGpuWrite(dst.base_address, size);
        rasterizer.NoteHleInOrderWrite(dst.base_address, size);
    }
    static std::atomic<u32> copies{0};
    const u32 n = copies.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 3 || (n & 65535) == 0) {
        std::printf("uc3: scalar copy in order (GOW3_HLE_INORDER): %u copies, 0x%llx -> 0x%llx "
                    "(%llu bytes)\n",
                    n, static_cast<unsigned long long>(src.base_address),
                    static_cast<unsigned long long>(dst.base_address),
                    static_cast<unsigned long long>(size));
    }
    return true;
}

// uc3 A4.1c: GOW3_HLE_BIG_MIN=<bytes> is the smallest copy GOW3_HLE_BIG_PIPE hands to the
// recording thread (default 1 MB).
unsigned long long BigPipeMin() {
    static const unsigned long long min = [] {
        const char* env = std::getenv("GOW3_HLE_BIG_MIN");
        const long long v = env ? std::strtoll(env, nullptr, 10) : 0;
        return v > 0 ? static_cast<unsigned long long>(v) : (1ull << 20);
    }();
    return min;
}

unsigned long long ScalarCopyBytes(const Shader::Info& info,
                                   const AmdGpu::ComputeProgram& cs_program,
                                   unsigned long long* dst_address) {
    u64 group_size = 0;
    if (info.pgm_hash == SCALAR_COPY_SHADER_HASH) {
        group_size = 1;
    } else if (info.pgm_hash == SCALAR_COPY64_SHADER_HASH) {
        group_size = 64;
    } else {
        return 0;
    }
    const Shader::BufferResource* guest[2]{};
    u32 guest_count = 0;
    for (const auto& buffer : info.buffers) {
        if (!buffer.IsSpecial()) {
            if (guest_count < 2) {
                guest[guest_count] = &buffer;
            }
            ++guest_count;
        }
    }
    const auto ud = info.UserData();
    if (guest_count != 2 || ud.size() <= 6) {
        return 0;
    }
    const auto src = guest[0]->GetSharp(info);
    const auto dst = guest[1]->GetSharp(info);
    if (dst_address) {
        *dst_address = dst.base_address;
    }
    return 4 * std::min<u64>({u64(cs_program.dim_x) * group_size, ud[6], src.num_records,
                              dst.num_records});
}

bool IsGuestMemoryShaderHLE(unsigned long long pgm_hash) {
    static std::atomic<bool> announced{false};
    if (ScalarCopyHLEEnabled() && !announced.exchange(true)) {
        std::printf("uc3: scalar copy HLE on (GOW3_SCALAR_COPY_HLE)\n");
    }
    return ScalarCopyHLEEnabled() &&
           (pgm_hash == SCALAR_COPY_SHADER_HASH || pgm_hash == SCALAR_COPY64_SHADER_HASH);
}

bool ExecuteShaderHLE(const Shader::Info& info, const AmdGpu::Regs& regs,
                      const AmdGpu::ComputeProgram& cs_program, Rasterizer& rasterizer) {
    switch (info.pgm_hash) {
    case COPY_SHADER_HASH:
        return ExecuteCopyShaderHLE(info, cs_program, rasterizer);
    case SCALAR_COPY_SHADER_HASH:
        return ScalarCopyHLEEnabled() && ExecuteScalarCopyHLE(info, cs_program, rasterizer, 1);
    case SCALAR_COPY64_SHADER_HASH:
        return ScalarCopyHLEEnabled() && ExecuteScalarCopyHLE(info, cs_program, rasterizer, 64);
    default:
        return false;
    }
}

} // namespace Vulkan
