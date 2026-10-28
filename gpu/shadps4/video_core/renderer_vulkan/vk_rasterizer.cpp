// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <mutex>
#include <unordered_set>
#include <unordered_map>
#include <x86intrin.h>
#include <cstdio>
#include <format>
#include <xxhash.h>
#include "video_core/renderer_vulkan/ui_composition.h"
#include "gow3_toggles.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "common/debug.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/resource.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_depth_stencil_state.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "gow3_threads.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/texture_cache.h"
#include <vk_mem_alloc.h>
#ifdef MemoryBarrier
#undef MemoryBarrier
#endif

namespace Vulkan {

extern std::atomic<u64> gow3_submit_cycles[6];
extern std::atomic<u64> gow3_submit_count;

// gow3 R.9.3: stage B time per sub-step (GOW3_STAGEB_PROFILE=1), printed with the pipe stats.
namespace {
enum StageBSlot : u32 {
    SbTask, SbLeaf, SbSpecial, SbDispatch, SbDraw, SbPrepare, SbBind, SbBegin, SbInputs,
    SbStamp, SbEager, SbDispHle, SbDispBind, SbDispRecord, SbBindBuffers, SbBindTextures,
    SbBindDma, SbObtain, SbInvalidate, SbAccessed, SbADraw, SbADispatch, SbAGfxPipe, SbACsPipe,
    SbAPost, SbAPRing, SbAPCopySrc, SbAPCopyMem, SbAPCheck, SbAPLeaves, SbAPWrite, SbAPResolve, SbAPSparse, SbAPFlush, SbRingPrep,
    SbDrPrelude, SbDrMotion, SbDrPush, SbDrDynState, SbDrSchedBegin, SbDrRecord, SbDrDump,
    SbDrReset, SbDrKick, SbAdHle, SbAdPost, SbAdRing, SbAdWrite, SbCount
};
constexpr const char* stageb_names[SbCount] = {"tasks", "leaf copies", "special", "dispatch",
                                               "draw", "draw:prepare", "draw:bind",
                                               "draw:begin", "draw:inputs", "ring stamp",
                                               "eager submit", "dispatch:hle",
                                               "dispatch:bind", "dispatch:record",
                                               "bind:buffers", "bind:textures", "bind:dma",
                                               "buf:obtain", "buf:invalidate", "buf:accessed",
                                               "A:draw", "A:dispatch", "A:gfx pipeline",
                                               "A:cs pipeline", "A:post", "P:ring", "P:copy flat", "P:copy mem", "P:gpu check", "P:leaf readback", "P:write", "P:resolve", "P:sparse copy", "P:flush", "ring prep (any runner)",
                                               "draw:prelude", "draw:motion", "draw:push",
                                               "draw:dyn state", "draw:sched begin",
                                               "draw:record", "draw:dump", "draw:reset",
                                               "draw:kick", "A:dispatch hle check",
                                               "A:post (dispatch)", "P:ring (dispatch)",
                                               "P:write (dispatch)"};
std::atomic<u64> stageb_cycles[SbCount];
std::atomic<u64> stageb_counts[SbCount];
std::atomic<u64> ring_bytes{0}; ///< R.12: added on the copy thread with GOW3_RING_PREP
bool RingDedup() {
    static const bool on = [] {
        const char* value = std::getenv("GOW3_RING_DEDUP");
        return value && value[0] == '1';
    }();
    return on;
}
bool StageBProfile() {
    static const bool on = std::getenv("GOW3_STAGEB_PROFILE") != nullptr;
    return on;
}
struct StageBTimer {
    u32 slot;
    u64 start;
    explicit StageBTimer(u32 s) : slot(s), start(StageBProfile() ? __rdtsc() : 0) {}
    ~StageBTimer() {
        Stop();
    }
    void Stop() {
        if (start) {
            stageb_cycles[slot].fetch_add(__rdtsc() - start, std::memory_order_relaxed);
            stageb_counts[slot].fetch_add(1, std::memory_order_relaxed);
            start = 0;
        }
    }
};
} // namespace

// ucndc: GOW3_MIP_FIX_TRACE=1: an image sampled while it is a render target in the same draw
// (with MIP_FIX the bloom reads mip N-1 and writes mip N of one image), once per image and mip
// range (sampled and target).
static bool FeedbackTraceFirst(u64 key) {
    static const bool on = std::getenv("GOW3_MIP_FIX_TRACE") != nullptr;
    if (!on) {
        return false;
    }
    static std::mutex mutex;
    static std::unordered_set<u64> seen;
    std::scoped_lock lock{mutex};
    return seen.size() < 400 && seen.insert(key).second;
}

static Shader::PushData MakeUserData(const AmdGpu::Regs& regs) {
    // TODO(roamic): Add support for multiple viewports and geometry shaders when ViewportIndex
    // is encountered and implemented in the recompiler.
    Shader::PushData push_data{};
    push_data.xoffset = regs.viewport_control.xoffset_enable ? regs.viewports[0].xoffset : 0.f;
    push_data.xscale = regs.viewport_control.xscale_enable ? regs.viewports[0].xscale : 1.f;
    push_data.yoffset = regs.viewport_control.yoffset_enable ? regs.viewports[0].yoffset : 0.f;
    push_data.yscale = regs.viewport_control.yscale_enable ? regs.viewports[0].yscale : 1.f;
    return push_data;
}

Rasterizer::Rasterizer(const Instance& instance_, Scheduler& scheduler_, Runtime& runtime_,
                       AmdGpu::Liverpool* liverpool_)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_}, page_manager{this},
      buffer_cache{instance, scheduler, runtime, liverpool_, texture_cache, page_manager},
      texture_cache{instance, scheduler, runtime, liverpool_, buffer_cache, page_manager},
      liverpool{liverpool_}, memory{Core::Memory::Instance()},
      pipeline_cache{instance, scheduler, liverpool, buffer_cache.GetSparsePageShift()},
      host_markers_enabled{EmulatorSettings.IsVkHostMarkersEnabled()},
      guest_markers_enabled{EmulatorSettings.IsVkGuestMarkersEnabled()} {
    // Before the rasterizer is bound: Liverpool enqueues buffers only once it sees it.
    draw_prep = std::make_unique<DrawPreparation>(pipeline_cache);
    scene_targets = std::make_unique<SceneTargets>(instance, scheduler, runtime, texture_cache);
    // Object motion first: it fixes the buffer addresses the motion shader variants embed.
    object_motion = std::make_unique<ObjectMotion>(instance, scheduler);
    camera_motion = std::make_unique<CameraMotion>(instance, scheduler, texture_cache, runtime);
    camera_motion->SetObjectMotion(object_motion.get());
    upscaler = std::make_unique<TemporalUpscaler>(instance, scheduler, texture_cache, runtime,
                                                  *camera_motion, *scene_targets);
    if (!EmulatorSettings.IsNullGPU()) {
        liverpool->BindRasterizer(this);
    }
    memory->SetRasterizer(this);

    GpuProfiler::Init(instance, scheduler);
    if (DrawPipeWanted()) {
        constant_ring = std::make_unique<ConstantRing>(instance, scheduler);
        // R.14 D1: with lazy fence kicks, stage B kicks unkicked fences when it runs dry.
        const DrawPipe::IdleHook idle_hook =
            Gow3Stats::lazy_fence_kick ? +[](void* context) {
                static_cast<Rasterizer*>(context)->scheduler.KickUnkickedSignals();
            }
                                       : nullptr;
        draw_pipe = std::make_unique<DrawPipe>(&RunDrawPacket, this, idle_hook);
        if (RingCopier::Wanted()) {
            ring_copier = std::make_unique<RingCopier>(
                [](void* context, VAddr address, const void* src, u8* dst, u64 size, u64 offset) {
                    auto* self = static_cast<Rasterizer*>(context);
                    if (src) {
                        std::memcpy(dst, src, size); // R.12: a flat, from its packet or arena
                    } else {
                        self->memory->CopySparseMemoryBacking(address, dst, size);
                    }
                    self->constant_ring->Flush(offset, size);
                },
                this);
            draw_pipe->copier = ring_copier.get();
            std::printf("gow3: constant ring copies on their own thread (R.9.12)\n");
        }
        // ucndc R.12 A2+A3 (Fable): GOW3_RING_PREP=1 - the copy thread checks, deduplicates and
        // allocates each packet's constants (stage A only lists them), and copies the flats.
        if (const char* env = std::getenv("GOW3_RING_PREP"); env && env[0] == '1') {
            ring_prep = ring_copier && ring_copier->EnablePrep(
                                           [](void* context, const RingCopier::PrepJob& job) {
                                               static_cast<Rasterizer*>(context)->RunPrepJob(job);
                                           });
            if (!ring_prep) {
                std::printf("Ring prep: GOW3_RING_PREP needs GOW3_RING_COPY_THREAD=1 and "
                            "GOW3_RING_COPY_STEAL=1 (one copy thread), ignored\n");
            } else if (const char* n = std::getenv("GOW3_RING_PREP_VERIFY"); n && n[0]) {
                ring_prep_verify = static_cast<u32>(std::strtoul(n, nullptr, 10));
                std::printf("Ring prep: stage B compares every %u ring bindings with guest "
                            "memory\n",
                            ring_prep_verify);
            }
        }
    }
    // gow3: this thread joins the texture binding helper before it changes image state.
    runtime.SetImageAccessHook(&JoinBindHelper, this);
    scheduler.SetSubmitCallback([this](Vulkan::SubmitInfo& info) {
        runtime.FlushBarriers();
        buffer_cache.SubmitPendingArenaBinds(info);
    });
}

Rasterizer::~Rasterizer() = default;

inline const AmdGpu::Regs& Rasterizer::Regs() const {
    return stage_regs ? *stage_regs : liverpool->regs;
}

inline AmdGpu::CbDbExtent Rasterizer::CbExtent(u32 index) const {
    return stage_regs ? pipe_cb_extent[index] : liverpool->last_cb_extent[index];
}

inline const AmdGpu::ComputeProgram& Rasterizer::CsRegs() const {
    return stage_cs ? *stage_cs : liverpool->GetCsRegs();
}

inline AmdGpu::CbDbExtent Rasterizer::DbExtent() const {
    return stage_regs ? pipe_db_extent : liverpool->last_db_extent;
}

namespace {
/// magic_enum::enum_contains, from a table: it scans every enumerator (per texture per draw).
bool IsKnownFormat(AmdGpu::DataFormat data_fmt, AmdGpu::NumberFormat num_fmt) {
    static const auto tables = [] {
        std::pair<std::array<bool, 256>, std::array<bool, 256>> t{};
        for (const auto value : magic_enum::enum_values<AmdGpu::DataFormat>()) {
            if (static_cast<u32>(value) < 256) {
                t.first[static_cast<u32>(value)] = true;
            }
        }
        for (const auto value : magic_enum::enum_values<AmdGpu::NumberFormat>()) {
            if (static_cast<u32>(value) < 256) {
                t.second[static_cast<u32>(value)] = true;
            }
        }
        return t;
    }();
    const u32 d = static_cast<u32>(data_fmt), n = static_cast<u32>(num_fmt);
    return d < 256 && n < 256 && tables.first[d] && tables.second[n];
}
} // namespace

bool Rasterizer::FilterDraw() {
    const auto& regs = Regs();
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::EliminateFastClear) {
        // Clears the render target if FCE is launched before any draws
        EliminateFastClear();
        return false;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::FmaskDecompress) {
        // TODO: check for a valid MRT1 to promote the draw to the resolve pass.
        LOG_TRACE(Render_Vulkan, "FMask decompression pass skipped");
        ScopedMarkerInsert("FmaskDecompress");
        EliminateFastClear();
        return false;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Resolve) {
        LOG_TRACE(Render_Vulkan, "Resolve pass");
        Resolve();
        return false;
    }
    if (regs.primitive_type == AmdGpu::PrimitiveType::None) {
        LOG_TRACE(Render_Vulkan, "Primitive type 'None' skipped");
        ScopedMarkerInsert("PrimitiveTypeNone");
        return false;
    }

    const bool cb_disabled =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    const auto depth_copy =
        regs.depth_render_override.force_z_dirty && regs.depth_render_override.force_z_valid &&
        regs.depth_buffer.DepthValid() && regs.depth_buffer.DepthWriteValid() &&
        regs.depth_buffer.DepthAddress() != regs.depth_buffer.DepthWriteAddress();
    const auto stencil_copy =
        regs.depth_render_override.force_stencil_dirty &&
        regs.depth_render_override.force_stencil_valid && regs.depth_buffer.StencilValid() &&
        regs.depth_buffer.StencilWriteValid() &&
        regs.depth_buffer.StencilAddress() != regs.depth_buffer.StencilWriteAddress();
    if (cb_disabled && (depth_copy || stencil_copy)) {
        // Games may disable color buffer and enable force depth/stencil dirty and valid to
        // do a copy from one depth-stencil surface to another, without a pixel shader.
        // We need to detect this case and perform the copy, otherwise it will have no effect.
        LOG_TRACE(Render_Vulkan, "Performing depth-stencil override copy");
        DepthStencilCopy(depth_copy, stencil_copy);
        return false;
    }

    return true;
}

template <typename... Parts>
VideoCore::ImageId Rasterizer::FindTargetMemoized(VideoCore::TextureCache::ImageDesc& desc,
                                                  LastTarget& last, auto&& make_desc,
                                                  const Parts&... parts) {
    static_assert((sizeof(Parts) + ...) <= 256, "target memo key too large");
    std::array<u8, 256> key{};
    u32 key_size = 0;
    const auto append = [&](const auto& part) {
        std::memcpy(key.data() + key_size, &part, sizeof(part));
        key_size += sizeof(part);
    };
    (append(parts), ...);
    const u64 generation = texture_cache.RegistryGeneration();
    const bool memo_enabled = !Gow3Toggle::Disabled(Gow3Toggle::TextureBindingMemo);
    if (memo_enabled && last.generation == generation && last.key_size == key_size &&
        std::memcmp(last.key.data(), key.data(), key_size) == 0) {
        texture_cache.MarkFound(last.image_id);
        return last.image_id;
    }
    const auto remember = [&](VideoCore::ImageId image_id) {
        last.key = key;
        last.key_size = key_size;
        last.generation = generation;
        last.image_id = image_id;
    };
    u64 hash = 0xCBF29CE484222325ull;
    for (u32 i = 0; i < key_size; i += 8) {
        u64 word;
        std::memcpy(&word, key.data() + i, 8);
        hash = (hash ^ word) * 0x100000001B3ull;
        hash ^= hash >> 29;
    }
    auto& memo = target_memo[hash % target_memo.size()];
    if (memo.generation == generation && memo.key_size == key_size &&
        std::memcmp(memo.key.data(), key.data(), key_size) == 0 && memo_enabled) {
        desc = memo.desc;
        texture_cache.MarkFound(memo.image_id);
        remember(memo.image_id);
        return memo.image_id;
    }
    make_desc();
    const VideoCore::ImageId image_id = texture_cache.FindImage(desc);
    if (generation == texture_cache.RegistryGeneration()) {
        memo.key = key;
        memo.key_size = key_size;
        memo.generation = generation;
        memo.image_id = image_id;
        memo.desc = desc;
        remember(image_id);
    } else {
        last.generation = ~0ULL;
    }
    return image_id;
}

void Rasterizer::PrepareRenderState(const GraphicsPipeline* pipeline) {
    // Prefetch render targets to handle overlaps with bound textures (e.g. mipgen)
    const auto& key = pipeline->GetGraphicsKey();
    const auto& regs = Regs();
    if (regs.color_control.degamma_enable) {
        LOG_WARNING(Render_Vulkan, "Color buffers require gamma correction");
    }

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    for (s32 cb = 0; cb < std::bit_width(key.mrt_mask); ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
        if (skip_cb_binding || !col_buf || !target_mask || (key.mrt_mask & (1 << cb)) == 0) {
            image_id = {};
            continue;
        }
        const auto& hint = CbExtent(cb);
        const u32 tag = 1;
        image_id = bound_images.emplace_back(FindTargetMemoized(
            desc, last_targets[cb], [&] { std::construct_at(&desc, col_buf, hint); }, tag,
            col_buf, hint));
        auto& image = texture_cache.GetImage(image_id);
        image.binding.is_target = 1u;
    }

    if (GetEffectiveDepthStencilState(regs).needs_attachment) {
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        const auto& hint = DbExtent();
        auto& [image_id, desc] = db_desc;
        const u32 tag = 2;
        image_id = bound_images.emplace_back(FindTargetMemoized(
            desc, last_targets[AmdGpu::NUM_COLOR_BUFFERS],
            [&] {
                std::construct_at(&desc, regs.depth_buffer, regs.depth_view, regs.depth_control,
                                  htile_address, hint);
            },
            tag, regs.depth_buffer, regs.depth_view, regs.depth_control, htile_address, hint));
        auto& image = texture_cache.GetImage(image_id);
        image.binding.is_target = 1u;
    } else {
        db_desc.first = {};
    }
    // gow3: the G-buffer pass (5+ color targets) holds the scene depth, and its constants the
    // main camera (shadow passes bind the same layout with the light's camera).
    gbuffer_draw = camera_motion->Enabled() && std::popcount(key.mrt_mask) >= 5 && db_desc.first;
    if (gbuffer_draw) {
        camera_motion->OnGBufferPass(db_desc.first);
    }
    if (upscaler->Enabled() && cb_descs[0].first) {
        upscaler->OnColorTarget(cb_descs[0].first);
    }
    // gow3: scene color: a full-size RGBA16F target drawn with the scene depth.
    if (upscaler->Enabled() && db_desc.first && db_desc.first == camera_motion->Depth() &&
        cb_descs[0].first && std::popcount(key.mrt_mask) <= 2) {
        const auto& color = texture_cache.GetImage(cb_descs[0].first);
        const auto& depth = texture_cache.GetImage(db_desc.first);
        if (color.info.pixel_format == vk::Format::eR16G16B16A16Sfloat &&
            color.info.size.width == depth.info.size.width &&
            color.info.size.height == depth.info.size.height) {
            upscaler->OnSceneColor(cb_descs[0].first);
            // Blended geometry without depth writes (not full-screen passes): transparents and
            // effects start. Blended layers writing depth (decals, wet/blood ground) are
            // surfaces and stay in the history.
            if (regs.blend_control[0].enable && !regs.color_buffers[0].info.blend_bypass &&
                !regs.depth_control.depth_write_enable &&
                (regs.num_indices > 6 || regs.num_instances.NumInstances() > 1)) {
                upscaler->OnBlendedSceneDraw();
            } else if (regs.num_indices <= 6 && regs.num_instances.NumInstances() <= 1) {
                // A full-screen pass over the scene (the fog composite): the transparents are in.
                upscaler->OnSceneComposite();
            }
        }
    }
}

static std::pair<u32, u32> GetDrawOffsets(
    const AmdGpu::Regs& regs, const Shader::Info& info,
    const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader) {
    u32 vertex_offset = regs.index_offset;
    u32 instance_offset = 0;
    if (fetch_shader) {
        if (vertex_offset == 0 && fetch_shader->vertex_offset_sgpr != -1) {
            vertex_offset = info.UserData()[fetch_shader->vertex_offset_sgpr];
        }
        if (fetch_shader->instance_offset_sgpr != -1) {
            instance_offset = info.UserData()[fetch_shader->instance_offset_sgpr];
        }
    }
    return {vertex_offset, instance_offset};
}

void Rasterizer::EliminateFastClear() {
    auto& col_buf = Regs().color_buffers[0];
    if (!col_buf || !col_buf.info.fast_clear) {
        return;
    }
    VideoCore::TextureCache::ImageDesc desc(col_buf, CbExtent(0));
    const auto image_id = texture_cache.FindImage(desc);
    const auto& image_view = texture_cache.FindRenderTarget(image_id, desc);
    if (!texture_cache.IsMetaCleared(col_buf.CmaskAddress(), col_buf.view.slice_start)) {
        return;
    }
    for (u32 slice = col_buf.view.slice_start; slice <= col_buf.view.slice_max; ++slice) {
        texture_cache.TouchMeta(col_buf.CmaskAddress(), slice, false);
    }
    auto& image = texture_cache.GetImage(image_id);
    const auto clear_value = LiverpoolToVK::ColorBufferClearValue(col_buf);

    ScopeMarkerBegin(fmt::format("EliminateFastClear:MRT={:#x}:M={:#x}", col_buf.Address(),
                                 col_buf.CmaskAddress()));
    runtime.ClearImage(&image, desc.view_info.range, clear_value);
    ScopeMarkerEnd();
}

// gow3: two-stage draw pipeline (vk_draw_pipe.h). Packet layout, 8-byte aligned parts:
// DrawPacket, u16 block indices, u32 block words (RegDirty::BlockWords each), then per stage a
// PacketStage with its user data and flattened user data, then (verification) the full file.
namespace {
enum class PacketKind : u32 { Draw, Task, Dispatch, Special, Indirect, IndirectDispatch };
struct TaskPacket {
    PacketKind kind;
    u32 size;
    Rasterizer::OrderedTask task;
};
struct DrawPacket {
    PacketKind kind;
    const Pipeline* pipeline;
    const PreparedDraw* prepared;
    u32 index_offset;
    u8 is_indexed;
    u8 regs_reset;
    u16 num_blocks;
    u32 num_stages;
    u32 verify;
    std::array<AmdGpu::CbDbExtent, AmdGpu::NUM_COLOR_BUFFERS> cb_extent;
    AmdGpu::CbDbExtent db_extent;
    u64 ring_end; ///< constant ring position after this packet's copies
};
struct PacketStage {
    const Shader::Info* info;
    VAddr pgm_base;
    u32 ud_size;
    u32 flat_size;
    u32 num_ring;
    u32 num_leaves; ///< uc3: LeafCopy entries after the ring bindings
    /// R.10 A1 v8 (Fable 2): a trusted draw's flat in its submission's arena (alive until
    /// stage B recorded the submission); null when the flat follows the user data inline.
    const u32* flat_ext;
};
constexpr u32 AlignPacket(u32 size) {
    return (size + 7) & ~7u;
}
u32 VerifyInterval() {
    static const u32 interval = [] {
        const char* env = std::getenv("GOW3_PIPE_VERIFY");
        return env ? static_cast<u32>(std::strtoul(env, nullptr, 10)) : 0u;
    }();
    return interval;
}
} // namespace

bool Rasterizer::DrawPipeWanted() {
    const char* env = std::getenv("GOW3_DRAW_PIPE");
    if (env && env[0]) {
        return env[0] == '1';
    }
    // Stage B spins while draws flow. Measured ahead with 16 threads (+19%) and with 4 cores /
    // 8 threads (taskset, Steam Deck-like: +18%).
    return Gow3Threads::Available() >= 8;
}

bool Rasterizer::UseDrawPipe() const {
    return draw_pipe && !host_markers_enabled && !Gow3Toggle::Disabled(Gow3Toggle::DrawPipeline);
}

bool Rasterizer::OnStageA() const {
#ifdef _WIN32
    // gow3 (Windows): libc++ compares std::thread::id through winpthreads' pthread_equal,
    // which asks the kernel (GetThreadId) for both threads: two system calls per check, many
    // per draw. The numeric thread id comes from the TEB.
    return static_cast<u32>(gettid()) == liverpool->GetGpuCommandProcessorThreadId();
#else
    return std::this_thread::get_id() == liverpool->GetGpuCommandProcessorThread();
#endif
}

namespace {
/// Stage A: cycles waited and drains that waited, per drain site (GOW3_FRAME_STATS), keyed by
/// function name and line.
struct DrainSite {
    u64 waited = 0;
    u64 count = 0;
};
std::unordered_map<const char*, std::unordered_map<u32, DrainSite>> drain_sites;
} // namespace

void Rasterizer::DrainDrawPipe(u32 reason, u32 line, const char* function) {
    if (!draw_pipe || !OnStageA()) {
        return;
    }
    static const bool stats = std::getenv("GOW3_FRAME_STATS") != nullptr;
    if (!stats) {
        draw_pipe->Drain(reason);
        return;
    }
    const u64 before = draw_pipe->drain_cycles;
    draw_pipe->Drain(reason);
    if (const u64 waited = draw_pipe->drain_cycles - before) {
        auto& site = drain_sites[function][line];
        site.waited += waited;
        ++site.count;
    }
}

bool Rasterizer::IsGpuSideThread() const {
    return OnStageA() || DrawPipe::OnStageB() || OnCaptureThread();
}

char Rasterizer::GpuSideThreadKind() const {
    return OnStageA() ? 'A' : DrawPipe::OnStageB() ? 'B' : OnCaptureThread() ? 'C' : '-';
}

namespace {
thread_local bool on_capture_thread = false;
}
void Rasterizer::MarkCaptureThread() {
    on_capture_thread = true;
}
bool Rasterizer::OnCaptureThread() noexcept {
    return on_capture_thread;
}

bool Rasterizer::IsGpuSideThreadId(u32 tid) const {
    // The userfaultfd thread handles the fault while the faulting thread waits: without locks
    // only if that thread owns the caches. The GPU command thread does not while the draw
    // recording thread may be running, so its faults take the locked path of guest threads.
    if (draw_pipe) {
        return tid == draw_pipe->StageBThreadId();
    }
    return tid == liverpool->GetGpuCommandProcessorThreadId();
}

bool Rasterizer::RingDedupOn() {
    return RingDedup();
}

void Rasterizer::NoteGuestWrite(VAddr address, u64 size) {
    SrtNoteWrite(address, size, SrtOriginGuestWrite);
    if (RingDedup() && size) {
        if (OnStageA() && ring_prep) {
            ring_copier->PushPrep({RingCopier::PrepKind::NoteWrite, nullptr, address, size});
        } else if (OnStageA()) {
            DedupNoteWrite(address, size);
        } else {
            dedup_offthread.fetch_add(1, std::memory_order_release);
        }
    }
}

void Rasterizer::DedupNoteWrite(VAddr address, u64 size) {
    ++dedup_wver;
    for (auto& write : dedup_writes) {
        if (address <= write.end && write.begin <= address + size) {
            write.begin = std::min(write.begin, address);
            write.end = std::max(write.end, address + size);
            write.ver = dedup_wver;
            return;
        }
    }
    if (dedup_writes.size() >= 64) {
        RingDedupClearNow();
    } else {
        dedup_writes.push_back({address, address + size, dedup_wver});
    }
}

void Rasterizer::NotePendingGpuWrite(VAddr address, u64 size) {
    SrtNoteWrite(address, size, SrtOriginPendingGpu);
    if (!draw_pipe || !size) {
        return;
    }
    if (RingDedup()) {
        if (ring_prep) {
            ring_copier->PushPrep({RingCopier::PrepKind::NoteWrite, nullptr, address, size});
        } else {
            DedupNoteWrite(address, size);
        }
    }
    // Draws write the same buffers over and over: an entry for the range only moves on.
    const u64 position = draw_pipe->Head();
    const VAddr end = address + size;
    for (auto& write : pending_writes) {
        if (address <= write.end && write.begin <= end) {
            write.begin = std::min(write.begin, address);
            write.end = std::max(write.end, end);
            write.position = position;
            pending_min = std::min(pending_min, address);
            pending_max = std::max(pending_max, end);
            return;
        }
    }
    pending_writes.push_back({address, end, position});
    pending_min = std::min(pending_min, address);
    pending_max = std::max(pending_max, end);
}

void Rasterizer::NoteHleInOrderWrite(VAddr address, u64 size) {
    if (!draw_pipe || !size) {
        return;
    }
    if (hle_inorder_writes.size() >= 256) {
        std::erase_if(hle_inorder_writes,
                      [&](const PendingWrite& write) { return draw_pipe->Reached(write.position); });
    }
    hle_inorder_writes.push_back({address, address + size, draw_pipe->Head()});
}

bool Rasterizer::HleInOrderPending(VAddr address, u64 size) {
    if (hle_inorder_writes.empty() || !OnStageA()) {
        return false;
    }
    const VAddr end = address + size;
    bool any_live = false;
    bool hit = false;
    for (const auto& write : hle_inorder_writes) {
        if (draw_pipe->Reached(write.position)) {
            continue;
        }
        any_live = true;
        if (address < write.end && write.begin < end) {
            hit = true;
            break;
        }
    }
    if (!any_live) {
        hle_inorder_writes.clear();
    }
    return hit;
}

bool Rasterizer::PendingWriteOverlaps(VAddr address, u64 size) {
    if (pending_writes.empty() || address >= pending_max || address + size <= pending_min) {
        return false;
    }
    // Now and then drop entries whose writes the recording thread has done: the bounds shrink.
    if ((++pending_checks & 63) == 0) {
        std::erase_if(pending_writes, [&](const PendingWrite& write) {
            return draw_pipe->Reached(write.position);
        });
        pending_min = ~VAddr{0};
        pending_max = 0;
        for (const auto& write : pending_writes) {
            pending_min = std::min(pending_min, write.begin);
            pending_max = std::max(pending_max, write.end);
        }
    }
    const VAddr end = address + size;
    for (const auto& write : pending_writes) {
        if (address < write.end && write.begin < end && !draw_pipe->Reached(write.position)) {
            return true;
        }
    }
    return false;
}

void Rasterizer::CollectRingBindings(const Shader::Info& stage, const PreparedDraw* prepared,
                           boost::container::static_vector<RingBinding, Shader::NUM_BUFFERS>& out,
                           std::vector<LeafCopy>& leaves) {
    const PreparedStage* prepared_stage = nullptr;
    if (prepared && !Gow3Toggle::Disabled(Gow3Toggle::PreparedResources)) {
        for (u32 i = 0; i < prepared->num_stages; ++i) {
            const auto& candidate = prepared->stages[i];
            if (&candidate.program->info == &stage &&
                candidate.num_buffers == stage.buffers.size()) {
                prepared_stage = &candidate;
                break;
            }
        }
    }
    const u64 alignment = std::max<u64>(instance.StorageMinAlignment(),
                                        instance.UniformMinAlignment());
    const auto copy = [&](u32 index, const void* source, VAddr address, u64 size) {
        const auto offset = constant_ring->Allocate(size, alignment);
        if (!offset) {
            Gow3Stats::uc3_ring_full.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        Gow3Stats::uc3_ring_copies.fetch_add(1, std::memory_order_relaxed);
        u8* dst = constant_ring->Data(*offset);
        StageBTimer copy_timer{source ? SbAPCopySrc : SbAPCopyMem};
        if (source) {
            std::memcpy(dst, source, size);
        } else {
            // uc3: a buffer the GPU wrote, bound as a constant: precise readbacks bring its data
            // back first (the other upload paths only copy CPU-modified or GPU-clean ranges).
            {
                StageBTimer t{SbAPResolve};
                memory->ResolveHostCopy(address, size);
            }
            ring_bytes.fetch_add(size, std::memory_order_relaxed);
            if (ring_copier) {
                // R.9.12: the copy thread copies and flushes; stage B waits for it.
                ring_copier->Push(address, dst, size, *offset);
                out.push_back({index, static_cast<u32>(size), *offset, address});
                return;
            }
            StageBTimer t{SbAPSparse};
            memory->CopySparseMemory(address, dst, size);
        }
        StageBTimer flush_timer{SbAPFlush};
        constant_ring->Flush(*offset, size);
        out.push_back({index, static_cast<u32>(size), *offset, address});
    };
    for (u32 index = 0; index < stage.buffers.size(); ++index) {
        const auto& desc = stage.buffers[index];
        if (desc.IsSpecial()) {
            const auto flat = stage.FlatUserData();
            if (desc.buffer_type == Shader::BufferType::Flatbuf && !flat.empty()) {
                const size_t before = out.size();
                copy(index, flat.data(), 0, flat.size() * sizeof(u32));
                if (out.size() != before) {
                    for (const auto& leaf : stage.gpu_leaves) {
                        leaves.push_back({out.back().offset + leaf.dst_dw * sizeof(u32), leaf.src});
                    }
                }
            }
            continue;
        }
        const auto vsharp = prepared_stage ? prepared_stage->buffer_sharps[index]
                                           : desc.GetSharp(stage);
        const VAddr address = vsharp.base_address;
        if (address == 0 || vsharp.GetSize() == 0) {
            continue;
        }
        const u64 size = memory->ClampRangeSize(address, vsharp.GetSize());
        if (desc.is_written) {
            NotePendingGpuWrite(address, size);
            if (OnComputeQueue()) {
                buffer_cache.NoteAcbWrite(address, size);
            }
            continue;
        }
        // The stream path of BufferCache::ObtainBuffer, taken here: small, read-only, not
        // written by the GPU (now or by work still queued for the recording thread).
        {
            StageBTimer check_timer{SbAPCheck};
            if (size == 0 || size > VideoCore::BufferCache::STREAM_THRESHOLD ||
                buffer_cache.IsRegionGpuModified(address, size) ||
                PendingWriteOverlaps(address, size)) {
                continue;
            }
        }
        if (!RingDedup()) {
            copy(index, nullptr, address, size);
            continue;
        }
        if (const u32 offthread = dedup_offthread.load(std::memory_order_acquire);
            offthread != dedup_offthread_seen) {
            dedup_offthread_seen = offthread;
            RingDedupClear();
        }
        // R.9.10: the same constants bound again in this submission: the earlier copy serves,
        // unless guest memory changed under it since (a pending GPU write over it, data read
        // back from the GPU, a CP copy) or the ring moved on past it.
        if (dedup_table.empty()) {
            dedup_table.resize(4096);
        }
        auto& entry = dedup_table[((address >> 4) ^ (address >> 16) ^ size) & 4095];
        if (entry.epoch == dedup_epoch && entry.address == address && entry.size == size) {
            bool clean = constant_ring->Position() - entry.abs < ConstantRing::Capacity / 2;
            if (clean && entry.wver != dedup_wver) {
                for (const auto& write : dedup_writes) {
                    if (write.ver > entry.wver && address < write.end &&
                        write.begin < address + size) {
                        clean = false;
                        break;
                    }
                }
            }
            if (clean) {
                dedup_hits.fetch_add(1, std::memory_order_relaxed);
                constant_ring->Pin(entry.abs);
                out.push_back({index, static_cast<u32>(size),
                               entry.abs % ConstantRing::Capacity, address});
                continue;
            }
            dedup_stale.fetch_add(1, std::memory_order_relaxed);
        } else {
            dedup_misses.fetch_add(1, std::memory_order_relaxed);
        }
        const size_t before = out.size();
        copy(index, nullptr, address, size);
        if (out.size() != before) {
            entry = {address, constant_ring->Position() - size, dedup_wver, 0,
                     static_cast<u32>(size), dedup_epoch};
        }
    }
}

void Rasterizer::CollectRingSlots(const Shader::Info& stage, const PreparedDraw* prepared,
                        boost::container::static_vector<RingBinding, Shader::NUM_BUFFERS>& out,
                        std::vector<LeafCopy>& leaves) {
    const PreparedStage* prepared_stage = nullptr;
    if (prepared && !Gow3Toggle::Disabled(Gow3Toggle::PreparedResources)) {
        for (u32 i = 0; i < prepared->num_stages; ++i) {
            const auto& candidate = prepared->stages[i];
            if (&candidate.program->info == &stage &&
                candidate.num_buffers == stage.buffers.size()) {
                prepared_stage = &candidate;
                break;
            }
        }
    }
    for (u32 index = 0; index < stage.buffers.size(); ++index) {
        const auto& desc = stage.buffers[index];
        if (desc.IsSpecial()) {
            const auto flat = stage.FlatUserData();
            if (desc.buffer_type == Shader::BufferType::Flatbuf && !flat.empty()) {
                // The flat: copied by the copy thread from the packet (or the A1 arena); its
                // GPU-written dwords are relative to it until the prepare places it.
                out.push_back({index, static_cast<u32>(flat.size() * sizeof(u32)), kPrepPending,
                               0});
                for (const auto& leaf : stage.gpu_leaves) {
                    leaves.push_back({leaf.dst_dw * sizeof(u32), leaf.src});
                }
            }
            continue;
        }
        const auto vsharp = prepared_stage ? prepared_stage->buffer_sharps[index]
                                           : desc.GetSharp(stage);
        const VAddr address = vsharp.base_address;
        if (address == 0 || vsharp.GetSize() == 0) {
            continue;
        }
        if (desc.is_written) {
            const u64 size = memory->ClampRangeSize(address, vsharp.GetSize());
            NotePendingGpuWrite(address, size);
            if (OnComputeQueue()) {
                buffer_cache.NoteAcbWrite(address, size);
            }
            continue;
        }
        // Writes queued for stage B are stage A's to know: those buffers are bound there.
        const u64 size = vsharp.GetSize();
        {
            StageBTimer check_timer{SbAPCheck};
            if (PendingWriteOverlaps(address, size)) {
                continue;
            }
        }
        out.push_back({index, static_cast<u32>(size), kPrepPending, address});
    }
}

void Rasterizer::RunPrepJob(const RingCopier::PrepJob& job) {
    switch (job.kind) {
    case RingCopier::PrepKind::Packet:
        PrepareRingPacket(job.packet);
        break;
    case RingCopier::PrepKind::NoteWrite:
        DedupNoteWrite(job.address, job.size);
        break;
    case RingCopier::PrepKind::DedupClear:
        RingDedupClearNow();
        break;
    }
}

u64 Rasterizer::PrepCopy(VAddr address, u64 size, u64 alignment) {
    const auto copy = [&]() -> u64 {
        const auto offset = constant_ring->Allocate(size, alignment);
        if (!offset) {
            Gow3Stats::uc3_ring_full.fetch_add(1, std::memory_order_relaxed);
            return kPrepSkipped;
        }
        Gow3Stats::uc3_ring_copies.fetch_add(1, std::memory_order_relaxed);
        ring_bytes.fetch_add(size, std::memory_order_relaxed);
        ring_copier->Push(address, constant_ring->Data(*offset), size, *offset);
        return *offset;
    };
    if (!RingDedup()) {
        return copy();
    }
    if (const u32 offthread = dedup_offthread.load(std::memory_order_acquire);
        offthread != dedup_offthread_seen) {
        dedup_offthread_seen = offthread;
        RingDedupClearNow();
    }
    // As in CollectRingBindings, with 16384 entries (one writer now, the copy thread).
    constexpr u32 Entries = 16384;
    if (dedup_table.empty()) {
        dedup_table.resize(Entries);
    }
    auto& entry = dedup_table[((address >> 4) ^ (address >> 16) ^ size) & (Entries - 1)];
    if (entry.epoch == dedup_epoch && entry.address == address && entry.size == size) {
        bool clean = constant_ring->Position() - entry.abs < ConstantRing::Capacity / 2;
        if (clean && entry.wver != dedup_wver) {
            for (const auto& write : dedup_writes) {
                if (write.ver > entry.wver && address < write.end && write.begin < address + size) {
                    clean = false;
                    break;
                }
            }
        }
        if (clean) {
            dedup_hits.fetch_add(1, std::memory_order_relaxed);
            constant_ring->Pin(entry.abs);
            return entry.abs % ConstantRing::Capacity;
        }
        dedup_stale.fetch_add(1, std::memory_order_relaxed);
    } else {
        dedup_misses.fetch_add(1, std::memory_order_relaxed);
    }
    const u64 offset = copy();
    if (offset != kPrepSkipped) {
        entry = {address, constant_ring->Position() - size, dedup_wver, 0, static_cast<u32>(size),
                 dedup_epoch};
    }
    return offset;
}

void Rasterizer::PrepareRingPacket(u8* data) {
    StageBTimer prep_timer{SbRingPrep};
    auto& packet = *reinterpret_cast<DrawPacket*>(data);
    u8* in = data + sizeof(DrawPacket) + AlignPacket(packet.num_blocks * sizeof(u16)) +
             packet.num_blocks * AmdGpu::RegDirty::BlockWords * sizeof(u32);
    const u64 alignment = std::max<u64>(instance.StorageMinAlignment(),
                                        instance.UniformMinAlignment());
    for (u32 i = 0; i < packet.num_stages; ++i) {
        const auto& stage = *reinterpret_cast<const PacketStage*>(in);
        in += sizeof(PacketStage);
        auto* bindings = reinterpret_cast<RingBinding*>(in);
        in += stage.num_ring * sizeof(RingBinding);
        auto* leaves = reinterpret_cast<LeafCopy*>(in);
        in += stage.num_leaves * sizeof(LeafCopy);
        const u32* const flat =
            stage.flat_ext ? stage.flat_ext : reinterpret_cast<const u32*>(in) + stage.ud_size;
        in += AlignPacket((stage.ud_size + (stage.flat_ext ? 0 : stage.flat_size)) * sizeof(u32));
        for (u32 j = 0; j < stage.num_ring; ++j) {
            auto& binding = bindings[j];
            if (binding.offset != kPrepPending) {
                continue;
            }
            if (binding.address == 0) {
                // The flat. Without room, stage B reads its GPU-written dwords back into the
                // packet's flat (it owns the caches; this thread cannot read them back).
                const auto offset = constant_ring->Allocate(binding.size, alignment);
                if (!offset) {
                    Gow3Stats::uc3_ring_full.fetch_add(1, std::memory_order_relaxed);
                    binding.offset = kPrepSkipped;
                    continue;
                }
                Gow3Stats::uc3_ring_copies.fetch_add(1, std::memory_order_relaxed);
                ring_copier->Push(0, constant_ring->Data(*offset), binding.size, *offset, flat);
                binding.offset = *offset;
                for (u32 k = 0; k < stage.num_leaves; ++k) {
                    leaves[k].ring_offset += *offset;
                }
                continue;
            }
            // The stream path of BufferCache::ObtainBuffer, as in CollectRingBindings.
            const u64 size = memory->ClampRangeSize(binding.address, binding.size);
            {
                StageBTimer check_timer{SbAPCheck};
                if (size == 0 || size > VideoCore::BufferCache::STREAM_THRESHOLD ||
                    buffer_cache.IsRegionGpuModified(binding.address, size)) {
                    binding.offset = kPrepSkipped;
                    continue;
                }
            }
            binding.size = static_cast<u32>(size);
            binding.offset = PrepCopy(binding.address, size, alignment);
        }
    }
    packet.ring_end = constant_ring->Position();
    DrawPipe::SetCopySeq(data, ring_copier->Enqueued());
}

void Rasterizer::VerifyRingBinding(const RingBinding& ring, const u32* flat) {
    static std::atomic<u64> count{0};
    if (count.fetch_add(1, std::memory_order_relaxed) % ring_prep_verify != 0) {
        return;
    }
    thread_local std::vector<u8> bytes;
    bytes.resize(ring.size);
    if (flat) {
        // The flat: the bytes the copy read (the packet's flat or the A1 arena).
        std::memcpy(bytes.data(), flat, ring.size);
    } else {
        memory->CopySparseMemoryBacking(ring.address, bytes.data(), ring.size);
    }
    prep_verify_compared.fetch_add(1, std::memory_order_relaxed);
    const u8* in_ring = constant_ring->Data(ring.offset);
    if (std::memcmp(bytes.data(), in_ring, ring.size) != 0) {
        const u64 differ = prep_verify_differ.fetch_add(1, std::memory_order_relaxed);
        if (differ < 10) {
            u32 first = 0;
            while (first < ring.size && bytes[first] == in_ring[first]) {
                ++first;
            }
            std::printf("Ring prep verify: %s %#llx (%u bytes, ring offset %#llx) differs from "
                        "byte %u\n",
                        flat ? "flat" : "buffer", static_cast<unsigned long long>(ring.address),
                        ring.size,
                        static_cast<unsigned long long>(ring.offset), first);
        }
    }
}

void Rasterizer::PostDraw(const Pipeline* pipeline, const PreparedDraw* used_prepared,
                          bool is_indexed, u32 index_offset, const AmdGpu::ComputeProgram* cs,
                          const IndirectDraw* indirect) {
    // Fable R.14 D0: dispatches count in their own slots (A:post, P:ring, P:write).
    StageBTimer post_timer{cs ? SbAdPost : SbAPost};
    auto& dirty = liverpool->pipe_dirty;
    if (!pipe_synced) {
        // Stage B has not run yet: its register copy starts as this one.
        pipe_regs = liverpool->regs;
        dirty.Clear();
        pipe_synced = true;
    }
    std::array<u16, AmdGpu::RegDirty::NumBlocks> blocks;
    u32 num_blocks = 0;
#ifdef __GLIBCXX__
    for (size_t block = dirty.blocks._Find_first(); block < dirty.blocks.size();
         block = dirty.blocks._Find_next(block)) {
        blocks[num_blocks++] = static_cast<u16>(block);
    }
#else
    dirty.ForEachBlock([&](u32 block) { blocks[num_blocks++] = static_cast<u16>(block); });
    // uc3 (GOW3_REGDIRTY_VERIFY=1, diagnostic): the word mask against the full bitset scan.
    static const bool verify_dirty = [] {
        const char* env = std::getenv("GOW3_REGDIRTY_VERIFY");
        return env && env[0] == '1';
    }();
    if (verify_dirty) {
        static std::atomic<u64> checked{0}, mismatches{0};
        u32 expected = 0;
        bool same = true;
        for (u32 block = 0; block < AmdGpu::RegDirty::NumBlocks; ++block) {
            if (dirty.blocks.test(block)) {
                same &= expected < num_blocks && blocks[expected] == block;
                ++expected;
            }
        }
        same &= expected == num_blocks;
        const u64 n = checked.fetch_add(1, std::memory_order_relaxed) + 1;
        if (!same && mismatches.fetch_add(1, std::memory_order_relaxed) < 5) {
            std::printf("uc3 REGDIRTY_VERIFY: mismatch on draw %llu: mask %u blocks, bitset %u\n",
                        static_cast<unsigned long long>(n), num_blocks, expected);
        }
        if ((n & ((u64{1} << 20) - 1)) == 0) {
            std::printf("uc3 REGDIRTY_VERIFY: %llu draws checked, %llu mismatches\n",
                        static_cast<unsigned long long>(n),
                        static_cast<unsigned long long>(mismatches.load()));
        }
    }
#endif
    const auto stages =
        pipeline ? pipeline->GetStages() : std::span<const Shader::Info* const>{};
    // Constants: copied here into the ring, the recording thread only binds them.
    thread_local std::array<boost::container::static_vector<RingBinding, Shader::NUM_BUFFERS>,
                            Shader::MaxStageTypes>
        rings;
    thread_local std::array<std::vector<LeafCopy>, Shader::MaxStageTypes> stage_leaves;
    u32 num_stages = 0;
    u32 size = sizeof(DrawPacket) + AlignPacket(num_blocks * sizeof(u16)) +
               num_blocks * AmdGpu::RegDirty::BlockWords * sizeof(u32);
    for (const auto* stage : stages) {
        if (stage) {
            auto& ring = rings[num_stages];
            auto& leaves = stage_leaves[num_stages];
            ring.clear();
            leaves.clear();
            if (constant_ring && !Gow3Toggle::Disabled(Gow3Toggle::ConstantRing)) {
                StageBTimer ring_timer{cs ? SbAdRing : SbAPRing};
                if (ring_prep) {
                    CollectRingSlots(*stage, used_prepared, ring, leaves);
                } else {
                    CollectRingBindings(*stage, used_prepared, ring, leaves);
                }
            }
            if (leaves.size() != stage->gpu_leaves.size()) {
                // The flat is not in the ring: its GPU-written dwords are read back here.
                Gow3Stats::uc3_leaf_noring.fetch_add(stage->gpu_leaves.size(),
                                                     std::memory_order_relaxed);
                bool reads_flat = false;
                for (const auto& desc : stage->buffers) {
                    reads_flat |= desc.buffer_type == Shader::BufferType::Flatbuf;
                }
                static std::atomic<int> traced{0};
                static const bool srt_trace = std::getenv("GOW3_SRT_TRACE") != nullptr;
                if (srt_trace && reads_flat && traced.fetch_add(1) < 10) {
                    const bool has_flatbuf = reads_flat;
                    std::printf("leaf copy: no ring for shader %016llx (stage %u): %zu leaves "
                            "(first dw %u), "
                                "%zu ring bindings, %zu copies, flatbuf binding %d, flat %zu dw\n",
                                (unsigned long long)stage->pgm_hash, u32(stage->sw_stage),
                                stage->gpu_leaves.size(),
                                stage->gpu_leaves[0].dst_dw, ring.size(), leaves.size(),
                                has_flatbuf, stage->FlatUserData().size());
                    for (const auto& desc : stage->buffers) {
                        const auto& f = desc.sharp_fetch;
                        std::printf("  buffer type %u summary %u mask %#x offsets %u %u %u %u\n",
                                    u32(desc.buffer_type), u32(f.summary), u32(f.load_mask),
                                    u32(f.offsets[0]), u32(f.offsets[1]), u32(f.offsets[2]),
                                    u32(f.offsets[3]));
                    }
                    std::printf("  %zu images, %zu samplers, %zu fmasks\n", stage->images.size(),
                                stage->samplers.size(), stage->fmasks.size());
                }
                leaves.clear();
                // Experiment (Fable R.8.2 follow-up): GOW3_LEAF_DEAD=1 skips the readback when
                // the stage has no flat buffer binding (the shader cannot read those dwords).
                static const bool dead = [] {
                    const char* value = std::getenv("GOW3_LEAF_DEAD");
                    return value && value[0] == '1';
                }();
                if (dead && !reads_flat) {
                    Gow3Stats::uc3_leaf_dead.fetch_add(stage->gpu_leaves.size(),
                                                       std::memory_order_relaxed);
                    const_cast<Shader::Info*>(stage)->gpu_leaves.clear();
                } else {
                    StageBTimer leaf_timer{SbAPLeaves};
                    const_cast<Shader::Info*>(stage)->ReadBackGpuLeaves();
                }
            }
            ++num_stages;
            // R.10 A1 v8: a trusted flat is read by stage B from the arena, not copied.
            const u32 inline_flat = stage->trusted_flat ? 0 : u32(stage->FlatUserData().size());
            size += sizeof(PacketStage) +
                    AlignPacket(u32(stage->user_data.size() + inline_flat) * sizeof(u32)) +
                    u32(ring.size() * sizeof(RingBinding)) +
                    u32(leaves.size() * sizeof(LeafCopy));
        }
    }
    if (cs) {
        size += AlignPacket(sizeof(AmdGpu::ComputeProgram));
    }
    if (indirect) {
        size += AlignPacket(sizeof(IndirectDraw));
    }
    const u32 interval = VerifyInterval();
    const bool verify = interval && (draw_pipe->packets % interval) == 0;
    if (verify) {
        size += sizeof(AmdGpu::Regs);
    }

    StageBTimer write_timer{cs ? SbAdWrite : SbAPWrite};
    u8* out = draw_pipe->Begin(size);
    u8* const start = out;
    auto& packet = *reinterpret_cast<DrawPacket*>(out);
    packet.kind = cs         ? (indirect ? PacketKind::IndirectDispatch : PacketKind::Dispatch)
                  : indirect ? PacketKind::Indirect
                  : pipeline ? PacketKind::Draw
                             : PacketKind::Special;
    packet.pipeline = pipeline;
    packet.prepared = used_prepared;
    packet.index_offset = index_offset;
    packet.is_indexed = is_indexed;
    packet.regs_reset = dirty.reset;
    packet.num_blocks = static_cast<u16>(num_blocks);
    packet.num_stages = num_stages;
    packet.verify = verify;
    packet.cb_extent = liverpool->last_cb_extent;
    packet.db_extent = liverpool->last_db_extent;
    // R.12: with GOW3_RING_PREP the copy thread writes it after preparing the packet.
    packet.ring_end = constant_ring && !ring_prep ? constant_ring->Position() : 0;
    out += sizeof(DrawPacket);
    std::memcpy(out, blocks.data(), num_blocks * sizeof(u16));
    out += AlignPacket(num_blocks * sizeof(u16));
    for (u32 i = 0; i < num_blocks; ++i) {
        std::memcpy(out,
                    liverpool->regs.reg_array.data() + blocks[i] * AmdGpu::RegDirty::BlockWords,
                    AmdGpu::RegDirty::BlockWords * sizeof(u32));
        out += AmdGpu::RegDirty::BlockWords * sizeof(u32);
    }
    u32 stage_index = 0;
    for (const auto* stage : stages) {
        if (!stage) {
            continue;
        }
        const auto& leaves = stage_leaves[stage_index];
        const auto& ring = rings[stage_index++];
        auto& header = *reinterpret_cast<PacketStage*>(out);
        const auto flat = stage->FlatUserData();
        const u32* const flat_ext = stage->trusted_flat;
        header = {stage,
                  stage->pgm_base,
                  static_cast<u32>(stage->user_data.size()),
                  static_cast<u32>(flat.size()),
                  static_cast<u32>(ring.size()),
                  static_cast<u32>(leaves.size()),
                  flat_ext};
        out += sizeof(PacketStage);
        std::memcpy(out, ring.data(), ring.size() * sizeof(RingBinding));
        out += ring.size() * sizeof(RingBinding);
        std::memcpy(out, leaves.data(), leaves.size() * sizeof(LeafCopy));
        out += leaves.size() * sizeof(LeafCopy);
        std::memcpy(out, stage->user_data.data(), stage->user_data.size_bytes());
        const u32 inline_flat = flat_ext ? 0 : u32(flat.size());
        if (inline_flat) {
            std::memcpy(out + stage->user_data.size_bytes(), flat.data(), inline_flat * sizeof(u32));
        }
        out += AlignPacket(u32(stage->user_data.size() + inline_flat) * sizeof(u32));
    }
    if (cs) {
        std::memcpy(out, cs, sizeof(AmdGpu::ComputeProgram));
        out += AlignPacket(sizeof(AmdGpu::ComputeProgram));
    }
    if (indirect) {
        std::memcpy(out, indirect, sizeof(IndirectDraw));
        out += AlignPacket(sizeof(IndirectDraw));
    }
    if (verify) {
        std::memcpy(out, &liverpool->regs, sizeof(AmdGpu::Regs));
        out += sizeof(AmdGpu::Regs);
    }
    dirty.Clear();
    if (ring_prep) {
        // R.12: every draw packet is prepared (its ring_end comes from there).
        ring_copier->PushPrep({RingCopier::PrepKind::Packet, start, 0, 0});
    }
    draw_pipe->Commit(static_cast<u32>(out - start));
    write_timer.Stop();
    PrintPipeStats();
}

void Rasterizer::RetireSubmission() {
    if (!draw_pipe || !OnStageA()) {
        return;
    }
    while (!pipe_keepalive.empty() && draw_pipe->Reached(pipe_keepalive.front().first)) {
        pipe_keepalive.pop_front();
    }
    if (!draw_pipe->Idle()) {
        pipe_keepalive.emplace_back(draw_pipe->Head(), draw_prep->KeepCurrent());
    }
}

// uc3 (GOW3_B_CENSUS=1, diagnostic, Fable R.14): dispatches recorded by stage B, per shader.
namespace {
struct DispatchCensus {
    u64 count = 0;
    u64 groups = 0;
    u64 indirect = 0;
    u64 after_draw = 0;
    u32 buffers = 0;
    u32 images = 0;
    u32 written = 0; ///< buffers + images the shader writes
};
std::mutex census_mutex;
std::unordered_map<u64, DispatchCensus> census_dispatches;
thread_local bool census_last_was_draw = false;
void NoteDispatchCensus(const Shader::Info& info, const AmdGpu::ComputeProgram& cs, bool indirect) {
    u32 written = 0;
    for (const auto& buffer : info.buffers) {
        written += buffer.is_written ? 1 : 0;
    }
    for (const auto& image : info.images) {
        written += image.is_written ? 1 : 0;
    }
    std::scoped_lock lock{census_mutex};
    auto& entry = census_dispatches[info.pgm_hash];
    ++entry.count;
    entry.groups += indirect ? 0 : u64(cs.dim_x) * cs.dim_y * cs.dim_z;
    entry.indirect += indirect ? 1 : 0;
    entry.after_draw += census_last_was_draw ? 1 : 0;
    entry.buffers = static_cast<u32>(info.buffers.size());
    entry.images = static_cast<u32>(info.images.size());
    entry.written = written;
    census_last_was_draw = false;
}
// Fable R.14 D0 (2, 3): the V#s of the two most frequent ACB dispatches. Distinct sets per
// window, and the buffers of the first 10 dispatches in two consecutive frames (once).
constexpr u64 kCensusShaders[2] = {0x8f067540ull, 0x7d7e7206ull};
struct SharpCensus {
    u64 dispatches = 0;
    u64 total = 0;
    std::unordered_set<u64> sets;
    u64 sample_frame = 0; ///< first sampled frame (0: not started)
    u32 sampled[2]{};     ///< dispatches printed in sample_frame and the next one
};
SharpCensus sharp_census[2];
std::mutex sharp_mutex;
void NoteSharpCensus(const Shader::Info& info, Rasterizer& rasterizer) {
    const int which = info.pgm_hash == kCensusShaders[0] ? 0 : info.pgm_hash == kCensusShaders[1] ? 1 : -1;
    if (which < 0) {
        return;
    }
    std::array<AmdGpu::Buffer, 16> sharps{};
    std::array<bool, 16> written{};
    u32 count = 0;
    for (const auto& buffer : info.buffers) {
        if (buffer.IsSpecial() || count == sharps.size()) {
            continue;
        }
        written[count] = buffer.is_written;
        sharps[count++] = buffer.GetSharp(info);
    }
    const u64 set = XXH3_64bits(sharps.data(), count * sizeof(AmdGpu::Buffer));
    const u64 frame = Gow3Stats::frame_ticks.load(std::memory_order_relaxed);
    std::scoped_lock lock{sharp_mutex};
    auto& census = sharp_census[which];
    ++census.dispatches;
    ++census.total;
    census.sets.insert(set);
    // Sample after the scene settled (about 10 s of play).
    if (census.sample_frame == 0 && census.total >= 300000) {
        census.sample_frame = frame + 1;
    }
    if (census.sample_frame == 0 || frame < census.sample_frame || frame > census.sample_frame + 1) {
        return;
    }
    const u32 slot = static_cast<u32>(frame - census.sample_frame);
    if (census.sampled[slot] >= 10) {
        return;
    }
    const u32 n = census.sampled[slot]++;
    std::printf("uc3 sharps %016llx frame +%u #%u set %016llx:",
                static_cast<unsigned long long>(info.pgm_hash), slot, n,
                static_cast<unsigned long long>(set));
    for (u32 i = 0; i < count; ++i) {
        const auto& sharp = sharps[i];
        const u64 size = sharp.GetSize();
        const bool gpu = sharp.base_address && size && rasterizer.IsGpuWritten(sharp.base_address, size);
        std::printf(" [%u %s %010llx stride %u records %u%s]", i, written[i] ? "W" : "R",
                    static_cast<unsigned long long>(sharp.base_address), sharp.GetStride(),
                    sharp.num_records, gpu ? " gpu" : "");
    }
    std::printf("\n");
}

void PrintBCensus(double seconds) {
    {
        std::scoped_lock lock{sharp_mutex};
        std::printf("  B census, V# sets (5 s window): ");
        for (int i = 0; i < 2; ++i) {
            auto& census = sharp_census[i];
            std::printf("%016llx %.0f dispatches/s, %zu distinct sets; ",
                        static_cast<unsigned long long>(kCensusShaders[i]),
                        census.dispatches / seconds, census.sets.size());
            census.dispatches = 0;
            census.sets.clear();
        }
        const u64 kicks = Gow3Stats::rec_kicks.exchange(0, std::memory_order_relaxed);
        const u64 chunks = Gow3Stats::rec_kick_chunks.exchange(0, std::memory_order_relaxed);
        const u64 bytes = Gow3Stats::rec_kick_bytes.exchange(0, std::memory_order_relaxed);
        const u64 wakes = Gow3Stats::rec_wakes.exchange(0, std::memory_order_relaxed);
        const u64 exec_ns = Gow3Stats::rec_exec_ns.exchange(0, std::memory_order_relaxed);
        std::printf("recorder: %.0f kicks/s, %.2f chunks and %.0f bytes per kick, %.0f%% woke "
                    "it, busy %.1f%%\n",
                    kicks / seconds, kicks ? double(chunks) / kicks : 0.0,
                    kicks ? double(bytes) / kicks : 0.0, kicks ? 100.0 * wakes / kicks : 0.0,
                    exec_ns / seconds / 1e7);
        const u64 kf = Gow3Stats::fence_kicks_forced.exchange(0, std::memory_order_relaxed);
        const u64 ff = Gow3Stats::fences_forced.exchange(0, std::memory_order_relaxed);
        const u64 kn = Gow3Stats::fence_kicks_natural.exchange(0, std::memory_order_relaxed);
        const u64 fn = Gow3Stats::fences_natural.exchange(0, std::memory_order_relaxed);
        const u64 idle = Gow3Stats::idle_kicks.exchange(0, std::memory_order_relaxed);
        if (Gow3Stats::d4_diag) {
            const u64 fc = Gow3Stats::fill_cpu.exchange(0, std::memory_order_relaxed);
            const u64 fg = Gow3Stats::fill_gpu.exchange(0, std::memory_order_relaxed);
            const u64 fo = Gow3Stats::fill_gpu_own_bytes.exchange(0, std::memory_order_relaxed);
            std::printf("  D4 fills to memory: %.0f/s on the CPU, %.0f/s on the GPU (%.0f/s with "
                        "their own bytes GPU-written)\n",
                        fc / seconds, fg / seconds, fo / seconds);
        }
        std::printf("  fence kicks (lazy %d, batch %d): forced %.0f/s carrying %.2f fences, "
                    "natural %.0f/s carrying %.2f, stage B idle kicks %.0f/s\n",
                    Gow3Stats::lazy_fence_kick, Gow3Stats::fence_batch, kf / seconds,
                    kf ? double(ff) / kf : 0.0, kn / seconds, kn ? double(fn) / kn : 0.0,
                    idle / seconds);
    }
    std::vector<std::pair<u64, DispatchCensus>> top;
    u64 total = 0;
    {
        std::scoped_lock lock{census_mutex};
        top.assign(census_dispatches.begin(), census_dispatches.end());
        census_dispatches.clear();
    }
    for (const auto& [hash, entry] : top) {
        total += entry.count;
    }
    std::ranges::sort(top, [](const auto& a, const auto& b) { return a.second.count > b.second.count; });
    std::printf("  B census, dispatches %.0f/s in %zu shaders; top (hash: /s, groups avg, "
                "buffers/images/written, %% after a draw, %% indirect):",
                total / seconds, top.size());
    for (size_t i = 0; i < top.size() && i < 10; ++i) {
        const auto& [hash, e] = top[i];
        const u64 direct = e.count - e.indirect;
        std::printf(" %016llx: %.0f, %.1f, %u/%u/%u, %.0f%%, %.0f%%;",
                    static_cast<unsigned long long>(hash), e.count / seconds,
                    direct ? double(e.groups) / direct : 0.0, e.buffers, e.images, e.written,
                    100.0 * e.after_draw / e.count, 100.0 * e.indirect / e.count);
    }
    std::printf("\n  B census, tasks (/s, us each):");
    for (auto& slot : Gow3Stats::task_stats) {
        const void* fn = slot.fn.load(std::memory_order_relaxed);
        if (!fn) {
            break;
        }
        const u64 n = slot.count.exchange(0, std::memory_order_relaxed);
        const u64 ns = slot.ns.exchange(0, std::memory_order_relaxed);
        if (!n) {
            continue;
        }
        if (slot.name) {
            std::printf(" %s %.0f %.2f;", slot.name, n / seconds, ns / 1e3 / n);
        } else {
            std::printf(" %p %.0f %.2f;", fn, n / seconds, ns / 1e3 / n);
        }
    }
    std::printf(" recording kicks from SignalAfterHostCopies %.0f/s\n",
                Gow3Stats::signal_after_copies_kicks.exchange(0, std::memory_order_relaxed) /
                    seconds);
}
} // namespace

bool Rasterizer::RunInOrder(OrderedTask task, const void* data, u32 size, u64 toggle) {
    if (!UseDrawPipe() || Gow3Toggle::Disabled(toggle)) {
        DrainDrawPipe(DrawPipe::ReasonRasterizer);
        task(*this, static_cast<const u8*>(data));
        return false;
    }
    const u32 total = sizeof(TaskPacket) + AlignPacket(size);
    u8* out = draw_pipe->Begin(total);
    *reinterpret_cast<TaskPacket*>(out) = {PacketKind::Task, size, task};
    std::memcpy(out + sizeof(TaskPacket), data, size);
    draw_pipe->Commit(total);
    return true;
}

void Rasterizer::RunDrawPacket(void* context, const u8* data, u32 size) {
    auto& self = *static_cast<Rasterizer*>(context);
    if (*reinterpret_cast<const PacketKind*>(data) == PacketKind::Task) {
        const auto& task = *reinterpret_cast<const TaskPacket*>(data);
        StageBTimer timer{SbTask};
        if (Gow3Stats::b_census) {
            Gow3Stats::diag_writer.store(reinterpret_cast<u64>(task.task), std::memory_order_relaxed);
            const auto start = std::chrono::steady_clock::now();
            task.task(self, data + sizeof(TaskPacket));
            if (auto* slot = Gow3Stats::TaskSlot(reinterpret_cast<const void*>(task.task))) {
                slot->count.fetch_add(1, std::memory_order_relaxed);
                slot->ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - start)
                                       .count(),
                                   std::memory_order_relaxed);
            }
            census_last_was_draw = false;
            return;
        }
        task.task(self, data + sizeof(TaskPacket));
        return;
    }
    const auto& packet = *reinterpret_cast<const DrawPacket*>(data);
    const u8* in = data + sizeof(DrawPacket);
    const auto* blocks = reinterpret_cast<const u16*>(in);
    in += AlignPacket(packet.num_blocks * sizeof(u16));
    auto& regs = self.pipe_regs;
    if (packet.regs_reset) {
        regs.SetDefaults();
    }
    for (u32 i = 0; i < packet.num_blocks; ++i) {
        std::memcpy(regs.reg_array.data() + blocks[i] * AmdGpu::RegDirty::BlockWords, in,
                    AmdGpu::RegDirty::BlockWords * sizeof(u32));
        in += AmdGpu::RegDirty::BlockWords * sizeof(u32);
    }
    ASSERT(packet.num_stages <= Shader::Info::MaxUdSnapshots);
    for (u32 i = 0; i < packet.num_stages; ++i) {
        const auto& stage = *reinterpret_cast<const PacketStage*>(in);
        in += sizeof(PacketStage);
        const auto* bindings = reinterpret_cast<const RingBinding*>(in);
        in += stage.num_ring * sizeof(RingBinding);
        self.ring_stages[i] = {stage.info, bindings, stage.num_ring,
                               reinterpret_cast<const LeafCopy*>(in), stage.num_leaves};
        in += stage.num_leaves * sizeof(LeafCopy);
        const auto* words = reinterpret_cast<const u32*>(in);
        const u32* const flat = stage.flat_ext ? stage.flat_ext : words + stage.ud_size;
        if (stage.num_leaves && self.ring_prep) {
            // R.12: the flat did not fit in the ring: its GPU-written dwords are read back into
            // the packet's flat here (a leaf's ring_offset is still relative to the flat).
            for (u32 j = 0; j < stage.num_ring; ++j) {
                if (bindings[j].address == 0 && bindings[j].offset == kPrepSkipped) {
                    ASSERT(!stage.flat_ext);
                    auto* const leaves = self.ring_stages[i].leaves;
                    for (u32 k = 0; k < stage.num_leaves; ++k) {
                        Shader::SrtReadDword(leaves[k].src, const_cast<u32*>(flat) +
                                                                leaves[k].ring_offset / sizeof(u32));
                    }
                    self.ring_stages[i].num_leaves = 0;
                    break;
                }
            }
        }
        Shader::Info::ud_snapshots[i] = {stage.info, words, stage.ud_size, flat,
                                         stage.flat_size, stage.pgm_base};
        in += AlignPacket((stage.ud_size + (stage.flat_ext ? 0 : stage.flat_size)) * sizeof(u32));
    }
    Shader::Info::num_ud_snapshots = packet.num_stages;
    self.num_ring_stages = packet.num_stages;
    const AmdGpu::ComputeProgram* cs = nullptr;
    if (packet.kind == PacketKind::Dispatch || packet.kind == PacketKind::IndirectDispatch) {
        cs = reinterpret_cast<const AmdGpu::ComputeProgram*>(in);
        in += AlignPacket(sizeof(AmdGpu::ComputeProgram));
    }
    const IndirectDraw* indirect = nullptr;
    if (packet.kind == PacketKind::Indirect || packet.kind == PacketKind::IndirectDispatch) {
        indirect = reinterpret_cast<const IndirectDraw*>(in);
        in += AlignPacket(sizeof(IndirectDraw));
    }
    if (packet.verify) {
        // The resource tables the GPU thread walked must read the same now: else a write
        // still queued here (WriteData, DMA) changed them after that thread read them.
        static u32 flat_reports = 0;
        for (u32 i = 0; i < packet.num_stages && flat_reports < 32; ++i) {
            const auto& snapshot = Shader::Info::ud_snapshots[i];
            const auto& srt = snapshot.info->srt_info;
            // Stages the pipeline selection did not refresh (no user data) keep their tables.
            if (!srt.walker_func || snapshot.flat_size == 0 || snapshot.user_data_size == 0) {
                continue;
            }
            std::vector<u32> flat(snapshot.flat_size);
            std::memcpy(flat.data(), snapshot.user_data,
                        std::min(snapshot.user_data_size, snapshot.flat_size) * sizeof(u32));
            // Pointers in the tables may be stale by now: a fault only skips the check.
            sigjmp_buf recover;
            if (sigsetjmp(recover, 0)) {
                runtime_fault_recover = nullptr;
                continue;
            }
            runtime_fault_recover = &recover;
            // The GPU-written dwords come out 0, as in the packet's flat (leaf copy).
            thread_local std::vector<Shader::SrtGpuLeaf> leaves;
            leaves.clear();
            Shader::RunSrtWalker(srt.walker_func, snapshot.user_data, flat.data(), flat.size(),
                                 false, &leaves);
            runtime_fault_recover = nullptr;
            if (std::memcmp(flat.data(), snapshot.flat, snapshot.flat_size * sizeof(u32)) != 0) {
                u32 first = 0, count = 0;
                for (u32 w = 0; w < snapshot.flat_size; ++w) {
                    if (flat[w] != snapshot.flat[w]) {
                        first = count++ ? first : w;
                    }
                }
                std::printf("Draw pipe: resource tables of shader %016llx changed after the GPU "
                            "thread read them (%u of %u words, first %u: %#x -> %#x, user data "
                            "%u words)\n",
                            static_cast<unsigned long long>(snapshot.info->pgm_hash), count,
                            snapshot.flat_size, first, snapshot.flat[first], flat[first],
                            snapshot.user_data_size);
                ++flat_reports;
            }
        }
        const auto* live = reinterpret_cast<const AmdGpu::Regs*>(in);
        in += sizeof(AmdGpu::Regs);
        static u32 reports = 0;
        for (u32 block = 0; block < AmdGpu::RegDirty::NumBlocks && reports < 32; ++block) {
            const u32 at = block * AmdGpu::RegDirty::BlockWords;
            if (std::memcmp(regs.reg_array.data() + at, live->reg_array.data() + at,
                            AmdGpu::RegDirty::BlockWords * sizeof(u32)) != 0) {
                for (u32 w = at; w < at + AmdGpu::RegDirty::BlockWords; ++w) {
                    if (regs.reg_array[w] != live->reg_array[w]) {
                        std::printf("Draw pipe: register copy differs at word %#x (%#x vs %#x)\n",
                                    w, regs.reg_array[w], live->reg_array[w]);
                        ++reports;
                        break;
                    }
                }
            }
        }
    }
    ASSERT(static_cast<u32>(in - data) == size);
    stage_regs = &regs;
    if (packet.kind != PacketKind::Special) {
        StageBTimer timer{SbLeaf};
        self.RecordLeafCopies();
    }
    self.pipe_cb_extent = packet.cb_extent;
    self.pipe_db_extent = packet.db_extent;
    if (packet.kind == PacketKind::Special) {
        // A draw FilterDraw handles (fast clear elimination, resolve, depth copy, skip).
        StageBTimer timer{SbSpecial};
        FrameCapture::Poll();
        self.scheduler.PopPendingOperations();
        self.FilterDraw();
    } else if (cs) {
        StageBTimer timer{SbDispatch};
        stage_cs = cs;
        const auto* pipeline = static_cast<const ComputePipeline*>(packet.pipeline);
        if (Gow3Stats::b_census) {
            NoteDispatchCensus(pipeline->GetStage(Shader::SwStage::Compute), *cs, indirect != nullptr);
            Gow3Stats::diag_writer.store(pipeline->GetStage(Shader::SwStage::Compute).pgm_hash,
                                         std::memory_order_relaxed);
        }
        if (indirect) {
            self.DispatchIndirectRecord(pipeline, indirect->args, indirect->stride);
        } else {
            self.DispatchRecord(pipeline);
        }
        stage_cs = nullptr;
    } else if (indirect) {
        StageBTimer timer{SbDraw};
        census_last_was_draw = true;
        Gow3Stats::diag_writer.store(1, std::memory_order_relaxed);
        self.DrawIndirectRecord(static_cast<const GraphicsPipeline*>(packet.pipeline),
                                packet.is_indexed, *indirect);
    } else {
        StageBTimer timer{SbDraw};
        census_last_was_draw = true;
        Gow3Stats::diag_writer.store(1, std::memory_order_relaxed);
        self.DrawRecord(static_cast<const GraphicsPipeline*>(packet.pipeline), packet.prepared,
                        packet.is_indexed, packet.index_offset);
    }
    Shader::Info::num_ud_snapshots = 0;
    self.num_ring_stages = 0;
    if (self.constant_ring) {
        StageBTimer timer{SbStamp};
        self.constant_ring->Stamp(packet.ring_end);
    }
    StageBTimer eager_timer{SbEager};
    self.MaybeEagerSubmit();
}

void Rasterizer::PipeCounters(u64& drain_cycles, u64& busy_cycles, u64& eager) const {
    drain_cycles = draw_pipe ? draw_pipe->drain_cycles : 0;
    busy_cycles = draw_pipe ? draw_pipe->busy_cycles.load(std::memory_order_relaxed) : 0;
    eager = eager_cuts.load(std::memory_order_relaxed);
}

void Rasterizer::MaybeEagerSubmit() {
    static const u32 every = [] {
        const char* env = std::getenv("GOW3_EAGER_SUBMIT");
        return env ? static_cast<u32>(std::max(0, std::atoi(env))) : 0u;
    }();
    if (every == 0) {
        return;
    }
    // rdtsc ticks per microsecond, measured once.
    static const u64 limit_tsc = [] {
        const char* env = std::getenv("GOW3_EAGER_SUBMIT_US");
        const u64 us = env ? std::max(0, std::atoi(env)) : 1500;
        const u64 t0 = __rdtsc();
        const auto c0 = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - c0 < std::chrono::milliseconds(2)) {
        }
        const u64 per_us = (__rdtsc() - t0) / 2000;
        return us * per_us;
    }();
    const u64 now = __rdtsc();
    if (eager_last_tsc == 0) {
        eager_last_tsc = now;
    }
    ++eager_packets;
    if (eager_packets < every && (limit_tsc == 0 || now - eager_last_tsc < limit_tsc)) {
        return;
    }
    if (!draw_pipe->MoreBehind()) {
        return; // the frame's end (or a pause): the submission at submit_done follows
    }
    eager_packets = 0;
    eager_last_tsc = now;
    eager_cuts.fetch_add(1, std::memory_order_relaxed);
    scheduler.FlushAsync();
}

void Rasterizer::PrintPipeStats() {
    static const bool stats = std::getenv("GOW3_FRAME_STATS") != nullptr;
    if (!stats || (draw_pipe->packets & 1023) != 0) {
        return;
    }
    static auto window = std::chrono::steady_clock::now();
    static u64 last_packets = 0, last_drains = 0, last_drain_cycles = 0, last_busy = 0;
    static u64 last_tsc = __rdtsc();
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - window).count();
    if (seconds < 5.0) {
        return;
    }
    const u64 tsc = __rdtsc();
    const double cycles = double(tsc - last_tsc);
    const u64 busy = draw_pipe->busy_cycles.load(std::memory_order_relaxed);
    std::printf("Draw pipe: %.0f draws/s pipelined, %.0f drains/s that waited, stage A waited "
                "%.1f%%, stage B busy %.1f%%\n",
                (draw_pipe->packets - last_packets) / seconds,
                (draw_pipe->drains - last_drains) / seconds,
                100.0 * (draw_pipe->drain_cycles - last_drain_cycles) / cycles,
                100.0 * (busy - last_busy) / cycles);
    static u64 last_cuts = 0;
    if (const u64 cuts = eager_cuts.load(std::memory_order_relaxed); cuts != last_cuts) {
        std::printf("  eager submissions: %.0f/s\n", (cuts - last_cuts) / seconds);
        last_cuts = cuts;
    }
    if (ring_copier) {
        static u64 last_wait = 0, last_waits = 0, last_copy_busy = 0, last_bwait = 0, last_bwaits = 0;
        const u64 wait = ring_copier->wait_cycles.load(std::memory_order_relaxed);
        const u64 waits = ring_copier->waits.load(std::memory_order_relaxed);
        const u64 bwait = ring_copier->b_wait_cycles.load(std::memory_order_relaxed);
        const u64 bwaits = ring_copier->b_waits.load(std::memory_order_relaxed);
        const u64 copy_busy = ring_copier->busy_cycles.load(std::memory_order_relaxed);
        std::printf("  ring copy thread: busy %.1f%%; stage B waited for it %.1f%% (%.0f/s), "
                    "others %.1f%% (%.0f/s)\n",
                    100.0 * (copy_busy - last_copy_busy) / cycles,
                    100.0 * (bwait - last_bwait) / cycles, (bwaits - last_bwaits) / seconds,
                    100.0 * (wait - last_wait) / cycles, (waits - last_waits) / seconds);
        last_wait = wait;
        last_waits = waits;
        last_bwait = bwait;
        last_bwaits = bwaits;
        last_copy_busy = copy_busy;
        if (ring_copier->PrepOn()) {
            static u64 l_cycles = 0, l_packets = 0, l_backlog = 0, l_drains = 0, l_bw = 0, l_aw = 0,
                       l_br = 0, l_ar = 0;
            const u64 pc = ring_copier->prep_cycles.load(std::memory_order_relaxed);
            const u64 pp = ring_copier->prep_packets.load(std::memory_order_relaxed);
            const u64 pb = ring_copier->prep_backlog.load(std::memory_order_relaxed);
            const u64 pd = ring_copier->prep_drains.load(std::memory_order_relaxed);
            const u64 bw = ring_copier->b_prep_waits.load(std::memory_order_relaxed);
            const u64 aw = ring_copier->a_prep_waits.load(std::memory_order_relaxed);
            const u64 br = ring_copier->b_prep_runs.load(std::memory_order_relaxed);
            const u64 ar = ring_copier->a_prep_runs.load(std::memory_order_relaxed);
            std::printf("  ring prep: %.2f us/packet, %.0f packets/s, mean backlog %.1f; waits "
                        "for a prepare: stage B %.0f/s, others %.0f/s; prepares run while "
                        "waiting: stage B %.0f/s, others %.0f/s\n",
                        pp != l_packets ? seconds * 1e6 * (pc - l_cycles) / cycles / (pp - l_packets)
                                        : 0.0,
                        (pp - l_packets) / seconds,
                        pd != l_drains ? double(pb - l_backlog) / (pd - l_drains) : 0.0,
                        (bw - l_bw) / seconds, (aw - l_aw) / seconds, (br - l_br) / seconds,
                        (ar - l_ar) / seconds);
            l_br = br;
            l_ar = ar;
            l_cycles = pc;
            l_packets = pp;
            l_backlog = pb;
            l_drains = pd;
            l_bw = bw;
            l_aw = aw;
            if (ring_prep_verify || prep_pending_seen.load(std::memory_order_relaxed)) {
                std::printf("  ring prep verify: %llu compared, %llu differ, %llu pending seen\n",
                            static_cast<unsigned long long>(prep_verify_compared.load()),
                            static_cast<unsigned long long>(prep_verify_differ.load()),
                            static_cast<unsigned long long>(prep_pending_seen.load()));
            }
        }
    }
    if (StageBProfile()) {
        const u64 bytes = ring_bytes.exchange(0, std::memory_order_relaxed);
        std::printf("  constant ring: %.1f MB/s, %.0f copies/s, %.0f bytes/copy\n",
                    bytes / seconds / 1e6, stageb_counts[SbAPSparse] / seconds,
                    stageb_counts[SbAPSparse] ? double(bytes) / stageb_counts[SbAPSparse] : 0.0);
        if (RingDedup()) {
            std::printf("  ring dedup: %.0f hits/s, %.0f misses/s, %.0f stale/s\n",
                        dedup_hits.exchange(0, std::memory_order_relaxed) / seconds,
                        dedup_misses.exchange(0, std::memory_order_relaxed) / seconds,
                        dedup_stale.exchange(0, std::memory_order_relaxed) / seconds);
        }
        {
            extern std::atomic<u64> g_gow3_gfx_pipe_split[11];
            const u64 pub = g_gow3_gfx_pipe_split[0].exchange(0, std::memory_order_relaxed);
            const u64 chk = g_gow3_gfx_pipe_split[1].exchange(0, std::memory_order_relaxed);
            const u64 calls = g_gow3_gfx_pipe_split[2].exchange(0, std::memory_order_relaxed);
            const u64 params = g_gow3_gfx_pipe_split[3].exchange(0, std::memory_order_relaxed);
            const u64 walk = g_gow3_gfx_pipe_split[4].exchange(0, std::memory_order_relaxed);
            const u64 cmp = g_gow3_gfx_pipe_split[5].exchange(0, std::memory_order_relaxed);
            const u64 changed = g_gow3_gfx_pipe_split[6].exchange(0, std::memory_order_relaxed);
            const u64 values = g_gow3_gfx_pipe_split[7].exchange(0, std::memory_order_relaxed);
            const u64 apply = g_gow3_gfx_pipe_split[8].exchange(0, std::memory_order_relaxed);
            const u64 apply_copy = g_gow3_gfx_pipe_split[9].exchange(0, std::memory_order_relaxed);
            const u64 apply_find = g_gow3_gfx_pipe_split[10].exchange(0, std::memory_order_relaxed);
            if (calls) {
                const double us = seconds * 1e6 / cycles / calls;
                std::printf("  A:gfx pipeline split (us/draw): publish+pump %.2f, prepared check %.2f "
                            "(GetParams %.2f, SRT walk %.2f, flat compare %.2f; trust: changed-since "
                            "%.2f, value compare %.2f, trusted apply %.2f (program check + flat copy "
                            "%.2f, pipeline find %.2f))\n",
                            us * pub, us * chk, us * params, us * walk, us * cmp, us * changed,
                            us * values, us * apply, us * apply_copy, us * apply_find);
            }
        }
        std::printf("  stage B by step (%% of time, us/call, k calls/s):");
        for (u32 i = 0; i < SbCount; ++i) {
            const u64 c = stageb_cycles[i].exchange(0, std::memory_order_relaxed);
            const u64 n = stageb_counts[i].exchange(0, std::memory_order_relaxed);
            if (n) {
                std::printf(" %s %.1f%% %.2f %.1fk", stageb_names[i], 100.0 * c / cycles,
                            seconds * 1e6 * c / cycles / n, n / seconds / 1000.0);
            }
        }
        std::printf("\n");
        if (Gow3Stats::b_census) {
            PrintBCensus(seconds);
        }
        const u64 submits = gow3_submit_count.exchange(0, std::memory_order_relaxed);
        if (submits) {
            static constexpr const char* steps[6] = {"on_submit", "end rendering",
                                                     "sync recording", "host copies",
                                                     "queue submit", "new cmdbuf+pending"};
            std::printf("  submissions %.0f/s, us/submission:", submits / seconds);
            for (u32 i = 0; i < 6; ++i) {
                const u64 c = gow3_submit_cycles[i].exchange(0, std::memory_order_relaxed);
                std::printf(" %s %.1f", steps[i], seconds * 1e6 * c / cycles / submits);
            }
            std::printf("\n");
        }
    }
    std::printf("  drains by reason/s:");
    for (u32 r = 0; r < DrawPipe::NumReasons; ++r) {
        if (const u64 n = draw_pipe->drains_by_reason[r]) {
            static constexpr const char* names[] = {"ConstRam", "commands", "compute",
                                                    "submission end", "draw", "rasterizer"};
            const double share = 100.0 * draw_pipe->cycles_by_reason[r] / cycles;
            draw_pipe->cycles_by_reason[r] = 0;
            if (r < 256) {
                std::printf(" %s=%.0f (%.1f%%)",
                            magic_enum::enum_name(AmdGpu::PM4ItOpcode(r)).data(), n / seconds,
                            share);
            } else {
                std::printf(" %s=%.0f (%.1f%%)", names[r - 256], n / seconds, share);
            }
        }
    }
    {
        // uc3 (Fable A4.2b): what the readback commands and the readback work cost.
        const auto take = [](std::atomic<u64>& v) { return v.exchange(0, std::memory_order_relaxed); };
        const u64 cmds = take(Gow3Stats::cmd_count), cmd_ns = take(Gow3Stats::cmd_ns);
        const u64 res = take(Gow3Stats::resolve_count), res_ns = take(Gow3Stats::resolve_ns);
        const u64 fl = take(Gow3Stats::flush_count), fl_ns = take(Gow3Stats::flush_ns);
        const u64 fl_wait = take(Gow3Stats::flush_wait_ns), fl_dl = take(Gow3Stats::flush_download_ns);
        const auto each = [](u64 ns, u64 n) { return n ? ns / 1e3 / n : 0.0; };
        std::printf("\n  readback commands: %.0f/s, %.1f us each, %.1f%% of a thread; resolves %.0f/s "
                    "%.1f us; flushes %.0f/s %.1f us (async wait %.1f, downloads %.1f, rest %.1f)",
                    cmds / seconds, each(cmd_ns, cmds), cmd_ns / seconds / 1e7, res / seconds,
                    each(res_ns, res), fl / seconds, each(fl_ns, fl), each(fl_wait, fl),
                    each(fl_dl, fl), each(fl_ns > fl_wait + fl_dl ? fl_ns - fl_wait - fl_dl : 0, fl));
        u64 rej[5], big[5];
        for (int i = 0; i < 5; ++i) {
            rej[i] = take(Gow3Stats::size_reject_hist[i]);
            big[i] = take(Gow3Stats::big_pipe_hist[i]);
        }
        std::printf("\n  scalar copy sizes/s (<=256, <=4K, <=64K, <=1M, >1M): refused for size "
                    "%.0f %.0f %.0f %.0f %.0f; handed to B %.0f %.0f %.0f %.0f %.0f",
                    rej[0] / seconds, rej[1] / seconds, rej[2] / seconds, rej[3] / seconds,
                    rej[4] / seconds, big[0] / seconds, big[1] / seconds, big[2] / seconds,
                    big[3] / seconds, big[4] / seconds);
    }
    // uc3: a site shows when it costs over 0.5% or waits 20 times a second or more.
    std::printf("\n  stage A waits over 0.5%% or 20/s (%% of time, waits/s):");
    for (const auto& [function, lines] : drain_sites) {
        for (const auto& [line, site] : lines) {
            if (site.waited * 200 > cycles || site.count >= 20 * seconds) {
                std::printf(" %s:%u %.1f%% %.0f/s", function, line, 100.0 * site.waited / cycles,
                            site.count / seconds);
            }
        }
    }
    std::printf("\n");
    drain_sites.clear();
    draw_pipe->drains_by_reason = {};
    window = now;
    last_tsc = tsc;
    last_packets = draw_pipe->packets;
    last_drains = draw_pipe->drains;
    last_drain_cycles = draw_pipe->drain_cycles;
    last_busy = busy;
}

bool Rasterizer::FilterDrawPasses() const {
    // FilterDraw's checks without its side effects: false when it would skip the draw or run
    // a pass of its own (fast clear elimination, resolve, depth/stencil copy).
    const auto& regs = Regs();
    using Mode = AmdGpu::ColorControl::OperationMode;
    const auto mode = regs.color_control.mode;
    if (mode == Mode::EliminateFastClear || mode == Mode::FmaskDecompress ||
        mode == Mode::Resolve || regs.primitive_type == AmdGpu::PrimitiveType::None) {
        return false;
    }
    const bool depth_copy =
        regs.depth_render_override.force_z_dirty && regs.depth_render_override.force_z_valid &&
        regs.depth_buffer.DepthValid() && regs.depth_buffer.DepthWriteValid() &&
        regs.depth_buffer.DepthAddress() != regs.depth_buffer.DepthWriteAddress();
    const bool stencil_copy =
        regs.depth_render_override.force_stencil_dirty &&
        regs.depth_render_override.force_stencil_valid && regs.depth_buffer.StencilValid() &&
        regs.depth_buffer.StencilWriteValid() &&
        regs.depth_buffer.StencilAddress() != regs.depth_buffer.StencilWriteAddress();
    return !(mode == Mode::Disable && (depth_copy || stencil_copy));
}

void Rasterizer::Draw(bool is_indexed, u32 index_offset, const PreparedDraw* prepared) {
    RENDERER_TRACE;
    StageBTimer a_timer{SbADraw};
    Gow3Stats::draws.fetch_add(1, std::memory_order_relaxed);

    // gow3: with the draw pipeline this thread only selects the pipeline and hands the draw
    // to the recording thread (DrawRecord there); draws FilterDraw handles itself run here.
    const bool pipelined = UseDrawPipe();
    if (pipelined && !FilterDrawPasses() &&
        !Gow3Toggle::Disabled(Gow3Toggle::PipelinedMemoryWrites)) {
        PostDraw(nullptr, nullptr, false, 0);
        return;
    }
    if (!pipelined || !FilterDrawPasses()) {
        DrainDrawPipe(DrawPipe::ReasonDraw);
        FrameCapture::Poll();
        scheduler.PopPendingOperations();
        if (!FilterDraw()) {
            return;
        }
    }
    StageBTimer pipe_timer{SbAGfxPipe};
    const GraphicsPipeline* pipeline = pipeline_cache.GetGraphicsPipeline({}, prepared);
    pipe_timer.Stop();
    const PreparedDraw* used_prepared = pipeline_cache.UsedPrepared();
    if (prepared) {
        draw_prep->Count(pipeline != nullptr &&
                         prepared->state.load(std::memory_order_acquire) == PreparedDraw::Ready &&
                         prepared->reg_checksum == liverpool->gfx_reg_checksum);
    }
    if (!pipeline) {
        Gow3Stats::uc3_no_pipeline_draws.fetch_add(1, std::memory_order_relaxed);
        static const bool skip_log = std::getenv("GOW3_SKIP_LOG") != nullptr;
        if (skip_log) {
            const auto& regs = Regs();
            for (u32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
                if (regs.color_buffers[cb] && regs.color_target_mask.GetMask(cb)) {
                    VideoCore::NoteSkippedTarget(regs.color_buffers[cb].Address());
                }
            }
        }
        return;
    }
    if (pipelined && FilterDrawPasses()) {
        PostDraw(pipeline, used_prepared, is_indexed, index_offset);
        return;
    }
    DrawRecord(pipeline, used_prepared, is_indexed, index_offset);
}

void Rasterizer::DrawRecord(const GraphicsPipeline* pipeline, const PreparedDraw* used_prepared,
                            bool is_indexed, u32 index_offset) {
    StageBTimer prelude_timer{SbDrPrelude};
    if (DrawPipe::OnStageB()) {
        FrameCapture::Poll();
        scheduler.PopPendingOperations();
    }
    const auto& regs = Regs();
    // gow3: the pass copying a finished frame to a display buffer; the previous draw's
    // target is that frame.
    if (camera_motion->Enabled() && regs.color_buffers[0] &&
        FrameCapture::IsDisplayBuffer(regs.color_buffers[0].Address())) {
        scene_started = false;
        camera_motion->OnDisplayPass(cb_descs[0].first);
        if (upscaler->OnFrameStart()) {
            object_motion->InvalidateHistory();
            camera_motion->InvalidateHistory();
        }
        camera_motion->SetJitter(upscaler->Jitter());
        object_motion->OnFrameStart();
        NoteFrameStart();
        static const char* scene_debug = std::getenv("GOW3_SCENE_DEBUG");
        scene_debug_frame = scene_debug && std::remove(scene_debug) == 0;
        scene_targets->debug = scene_debug_frame;
    } else if (GpuProfiler::Get() && regs.color_buffers[0] &&
               FrameCapture::IsDisplayBuffer(regs.color_buffers[0].Address())) {
        // uc3: without the upscaler the camera motion is off, but the GPU profile still needs
        // the frame boundary (the display pass) to collect.
        NoteFrameStart();
    }
    bind_prepared = used_prepared;
    motion_draw = pipeline->GetGraphicsKey().motion_vectors;
    motion_geometry = 0;

    prelude_timer.Stop();
    {
        StageBTimer timer{SbPrepare};
        PrepareRenderState(pipeline);
    }
    if (upscaler->Enabled() && std::popcount(pipeline->GetGraphicsKey().mrt_mask) == 1) {
        const auto& viewport = Regs().viewports[0];
        upscaler->OnDraw(pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash,
                         cb_descs[0].first, db_desc.first,
                         UiComposition::NativeViewport(viewport.xscale * 2, viewport.yscale * 2));
    }
    const PreparedDraw* draw_prepared = bind_prepared;
    // gow3: vertex and index buffers are resolved while the helper binds textures (their
    // commands are recorded after BeginRendering, as before).
    draw_inputs = {pipeline, draw_prepared, index_offset, is_indexed, true, false};
    bool bound;
    {
        StageBTimer timer{SbBind};
        bound = BindResources(pipeline);
    }
    bind_prepared = nullptr; // indirect draws and dispatches bind without prepared sharps
    const bool inputs_resolved = draw_inputs.resolved;
    draw_inputs.pending = false;
    if (!bound) {
        return;
    }
    const auto state = [&] {
        StageBTimer timer{SbBegin};
        return BeginRendering(pipeline);
    }();

    {
        StageBTimer timer{SbInputs};
        if (!inputs_resolved) {
            ResolveVertexBuffers(pipeline, draw_prepared);
            if (is_indexed) {
                ResolveIndexBuffer(index_offset);
            }
        }
        EmitVertexBuffers();
        if (is_indexed) {
            EmitIndexBuffer();
        }

        if (needs_barrier) {
            runtime.FlushBarriers();
        }
    }

    StageBTimer motion_timer{SbDrMotion};
    // gow3: screen-space (clip disabled) draws into the upscaler's output-size images.
    push_data.xscale *= target_scale[0];
    push_data.xoffset *= target_scale[0];
    push_data.yscale *= target_scale[1];
    push_data.yoffset *= target_scale[1];
    if (motion_draw && motion_geometry) {
        const auto& vs = pipeline->GetStage(Shader::SwStage::Vertex);
        const auto [base_vertex, first_instance] =
            GetDrawOffsets(regs, vs, pipeline->GetFetchShader());
        const u32 index_size =
            regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16 ? 2u : 4u;
        const VAddr index_address = is_indexed
            ? regs.index_base_address.Address<VAddr>() + u64(index_offset) * index_size : 0;
        // Without a character-size skeleton the draw is gated on its bone palette changing
        // since last frame (model constants follow the camera, the palette does not). The
        // check comes first: most gated draws are static world pieces with long index lists.
        static const bool all_motion = [] {
            const char* value = std::getenv("GOW3_OBJECT_MOTION_ALL");
            return value && value[0] == '1';
        }();
        bool gated = !all_motion;
        u64 palette = 0;
        for (const auto& resource : vs.buffers) {
            if (resource.IsSpecial()) continue;
            const auto buffer = resource.GetSharp(vs);
            const u32 size = buffer.GetSize();
            if (!buffer.Valid() || buffer.GetStride() != 16) continue;
            const auto role = Motion::ClassifyBuffer(size);
            if (role == Motion::BufferRole::Skeleton) {
                gated = false;
                break;
            }
            const VAddr address = buffer.base_address;
            if (role == Motion::BufferRole::SmallSkeleton &&
                memory->IsValidGpuMapping(address, 0) &&
                memory->ClampRangeSize(address, size) == size) {
                palette = XXH3_64bits_withSeed(reinterpret_cast<const void*>(address), size,
                                               palette);
            }
        }
        push_data.motion_param = 0;
        if (!gated || object_motion->Moving({.shader = vs.pgm_hash,
                                             .geometry = motion_geometry,
                                             .indices = index_address,
                                             .index_count = regs.num_indices,
                                             .instances = regs.num_instances.NumInstances(),
                                             .first_instance = first_instance},
                                            palette)) {
            Motion::VertexRange range{base_vertex, regs.num_indices};
            u64 topology = 0;
            if (is_indexed) {
                range = {};
                const u64 bytes = u64(regs.num_indices) * index_size;
                if (index_address && memory->IsValidGpuMapping(index_address, 0) &&
                    memory->ClampRangeSize(index_address, bytes) == bytes) {
                    const bool restart = regs.enable_primitive_restart != 0;
                    const u32 count = regs.num_indices;
                    const auto scanned = object_motion->IndexRange(
                        {index_address, count, index_size, s32(base_vertex), restart}, [&] {
                            const auto* data = reinterpret_cast<const void*>(index_address);
                            const auto scan_range =
                                index_size == 2
                                    ? Motion::IndexedRange(
                                          std::span(static_cast<const u16*>(data), count),
                                          s32(base_vertex), restart)
                                    : Motion::IndexedRange(
                                          std::span(static_cast<const u32*>(data), count),
                                          s32(base_vertex), restart);
                            return Motion::IndexRangeCache::Result{scan_range,
                                                                   XXH3_64bits(data, bytes)};
                        });
                    range = scanned.range;
                    topology = scanned.topology;
                }
            }
            push_data.motion_param = object_motion->PrepareDraw({
                .shader = vs.pgm_hash,
                .geometry = motion_geometry,
                .indices = index_address,
                .topology = topology,
                .index_count = regs.num_indices,
                .instances = regs.num_instances.NumInstances(),
                .first_instance = first_instance,
                .vertices = range,
            });
        }
    }
    motion_timer.Stop();
    StageBTimer push_timer{SbDrPush};
    pipeline->BindResources(set_writes, push_data);
    push_timer.Stop();
    // gow3: jitter geometry drawn with the scene depth, not full-screen passes (a shifted
    // full-screen quad leaves an edge column unwritten).
    draw_jitter = {};
    if (upscaler->Enabled() && db_desc.first && db_desc.first == camera_motion->Depth() &&
        (regs.num_indices > 6 || regs.num_instances.NumInstances() > 1)) {
        draw_jitter = upscaler->Jitter();
    }
    StageBTimer dyn_timer{SbDrDynState};
    UpdateDynamicState(pipeline, is_indexed);
    MarkPass(pipeline, state);
    dyn_timer.Stop();
    StageBTimer begin_timer{SbDrSchedBegin};
    scheduler.BeginRendering(state);
    begin_timer.Stop();
    StageBTimer record_timer{SbDrRecord};

    const auto& vs_info = pipeline->GetStage(Shader::SwStage::Vertex);
    const auto& fetch_shader = pipeline->GetFetchShader();
    const auto [vertex_offset, instance_offset] = GetDrawOffsets(regs, vs_info, fetch_shader);

    const vk::Pipeline handle = pipeline->Handle();
    const vk::Pipeline square = SquarePass(*pipeline);
    const u32 num_indices = regs.num_indices;
    const u32 num_instances = regs.num_instances.NumInstances();
    const u32 first_vertex = vertex_offset;
    const u32 first_instance = instance_offset;
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        const auto draw = [&] {
            if (is_indexed) {
                cmdbuf.drawIndexed(num_indices, num_instances, 0, s32(first_vertex),
                                   first_instance);
            } else {
                cmdbuf.draw(num_indices, num_instances, first_vertex, first_instance);
            }
        };
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, handle);
        draw();
        if (square) [[unlikely]] {
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, square);
            draw();
        }
    });
    record_timer.Stop();
    StageBTimer dump_timer{SbDrDump};
    if (FrameCapture::Active()) {
        const auto* ps = pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
        FrameCapture::Draw(vs_info.pgm_hash, ps ? ps->pgm_hash : 0, num_indices, num_instances);
        {
            // uc3 (sand diagnosis): the color write masks (4 bits per target, RGBA from bit 0).
            char note[64];
            std::snprintf(note, sizeof(note), "\n  target mask %08x", regs.color_target_mask.raw);
            FrameCapture::Note(note);
        }
        if (const auto& bc = regs.blend_control[0]; bc.enable && regs.color_buffers[0]) {
            char note[128];
            std::snprintf(note, sizeof(note), "\n  blend ps %08x idx %u: src %u dst %u func %u z %u%u",
                          u32(ps ? ps->pgm_hash : 0), num_indices, u32(bc.color_src_factor),
                          u32(bc.color_dst_factor), u32(bc.color_func),
                          u32(regs.depth_control.depth_enable),
                          u32(regs.depth_control.depth_write_enable));
            FrameCapture::Note(note);
        }
    }
    DebugState.IncDrawCall();
    DumpOnDraw(pipeline);
    SamplerLog(pipeline);
    dump_timer.Stop();

    {
        StageBTimer timer{SbDrReset};
        ResetBindings(false);
    }
    StageBTimer kick_timer{SbDrKick};
    scheduler.KickRecording();
}

void Rasterizer::DrawIndirect(bool is_indexed, VAddr arg_address, u32 offset, u32 stride,
                              u32 max_count, VAddr count_address, u16 vertex_sgpr_offset,
                              u16 instance_sgpr_offset) {
    RENDERER_TRACE;
    // gow3: like direct draws, handed to the recording thread after the pipeline selection
    // (else the GPU thread waited here for a whole frame of queued draws).
    const bool pipelined = UseDrawPipe() && FilterDrawPasses() &&
                           !Gow3Toggle::Disabled(Gow3Toggle::PipelinedIndirectDraws);
    if (!pipelined) {
        DrainDrawPipe();
        scheduler.PopPendingOperations();
        if (!FilterDraw()) {
            return;
        }
    }

    const DrawIndirectParams params = {
        .vertex_sgpr_offset = vertex_sgpr_offset,
        .instance_sgpr_offset = instance_sgpr_offset,
        .is_indirect = true,
    };
    Gow3Stats::uc3_indirect_draws.fetch_add(1, std::memory_order_relaxed);
    if (FrameCapture::Active()) {
        // uc3: bring the GPU-written arguments back into guest memory (the GPU thread waits
        // for the work before them), so the frame analyzer shows what the draw will use.
        ReadMemory(arg_address + offset, 20, true);
    }
    const GraphicsPipeline* pipeline = pipeline_cache.GetGraphicsPipeline(params);
    if (!pipeline) {
        Gow3Stats::uc3_no_pipeline_indirect.fetch_add(1, std::memory_order_relaxed);
        static const bool skip_log = std::getenv("GOW3_SKIP_LOG") != nullptr;
        if (skip_log) {
            const auto& regs = Regs();
            for (u32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
                if (regs.color_buffers[cb] && regs.color_target_mask.GetMask(cb)) {
                    VideoCore::NoteSkippedTarget(regs.color_buffers[cb].Address());
                }
            }
        }
        return;
    }
    const IndirectDraw indirect = {arg_address + offset, count_address, stride, max_count};
    if (pipelined) {
        PostDraw(pipeline, nullptr, is_indexed, 0, nullptr, &indirect);
        return;
    }
    DrawIndirectRecord(pipeline, is_indexed, indirect);
}

// uc3: GOW3_DUMP_PS=<hex ps hash> with GOW3_DUMP_TRIGGER=<file>: when the file exists, the
// color targets of the next draw (direct or indirect) with that pixel shader are written raw
// (Vulkan linear layout, num_bits per pixel) to GOW3_DUMP_DIR, and GOW3_DUMP_MEM=<hex addr>:<hex
// size> guest memory with them (cave torch light debugging).
// uc3 (GOW3_DUMP_CAPTURE=<ps>[:<addr>/<addr>...]+...+display[:...], ';' also separates; diagnosis): in every frame the
// frame analyzer records (GOW3_CAPTURE_FRAMES), the first draw of each listed pixel shader
// dumps its color targets and the images at the listed addresses, as GOW3_DUMP_PS does;
// "display" is the display pass that ends the frame. Files are named f<frame>_<ps>_...
struct CaptureDumpPoint {
    u64 ps = 0; ///< 0: the display pass
    std::string images;
    u32 done_frame = 0;
};

static std::vector<CaptureDumpPoint>& CaptureDumpPoints() {
    static std::vector<CaptureDumpPoint> points = [] {
        std::vector<CaptureDumpPoint> list;
        const char* env = std::getenv("GOW3_DUMP_CAPTURE");
        std::string spec = env ? env : "";
        size_t start = 0;
        while (start < spec.size()) {
            size_t end = spec.find_first_of(";+", start);
            if (end == std::string::npos) {
                end = spec.size();
            }
            const std::string item = spec.substr(start, end - start);
            start = end + 1;
            if (item.empty()) {
                continue;
            }
            const size_t colon = item.find(':');
            const std::string name = item.substr(0, colon);
            CaptureDumpPoint point;
            point.ps = name == "display" ? 0 : std::strtoull(name.c_str(), nullptr, 16);
            if (colon != std::string::npos) {
                point.images = item.substr(colon + 1);
            }
            list.push_back(std::move(point));
        }
        return list;
    }();
    return points;
}

void Rasterizer::DumpOnDraw(const GraphicsPipeline* pipeline) {
    static const char* dump_ps = std::getenv("GOW3_DUMP_PS");
    static const char* dump_trigger = std::getenv("GOW3_DUMP_TRIGGER");
    static int dump_index = 0;
    // uc3: nothing to dump (no GOW3_DUMP_PS trigger, no capture points): leave before any work.
    if (!(dump_ps && dump_trigger) && CaptureDumpPoints().empty()) {
        return;
    }
    const auto* dps = pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
    // uc3: read once. A getenv per draw cost ~2.2 us of stage B (v0.3.4 Cidadela 48 -> 37 FPS).
    static const char* dump_images = std::getenv("GOW3_DUMP_IMAGES");
    const char* image_list = dump_images;
    std::string tag_prefix;
    bool fire = false;
    if (auto& points = CaptureDumpPoints(); !points.empty() && FrameCapture::DumpFrame() != 0) {
        const u32 frame = FrameCapture::DumpFrame();
        const u64 hash = dps ? dps->pgm_hash : 0;
        for (auto& point : points) {
            const bool match =
                point.ps == 0 ? FrameCapture::InDisplayPass() : (dps && point.ps == hash);
            if (match && point.done_frame != frame) {
                point.done_frame = frame;
                image_list = point.images.c_str();
                tag_prefix = std::format("f{}_{:08x}_", frame, point.ps == 0 ? 0 : u32(hash));
                if (point.ps == 0) {
                    tag_prefix = std::format("f{}_display_", frame);
                }
                fire = true;
                break;
            }
        }
    }
    if (!fire) {
        fire = dump_ps && dump_trigger && dps &&
               dps->pgm_hash == std::strtoull(dump_ps, nullptr, 16) &&
               std::remove(dump_trigger) == 0;
    }
    if (fire) {
        const char* dir = std::getenv("GOW3_DUMP_DIR");
        const std::string base = dir && *dir ? dir : "out/dump";
        scheduler.EndRendering();
        // GOW3_DUMP_MIPS=1: every mip of each image, not only mip 0 (files _m<mip> from mip 1 on).
        static const bool dump_mips = [] {
            const char* v = std::getenv("GOW3_DUMP_MIPS");
            return v && v[0] == '1';
        }();
        const auto dump_image = [&](VideoCore::Image& image, const char* tag) {
          const u32 mips = dump_mips ? u32(image.info.resources.levels) : 1u;
          for (u32 mip = 0; mip < mips; ++mip) {
            if (image.info.props.is_depth) {
                return;
            }
            const u32 w = std::max(image.info.size.width >> mip, 1u),
                      h = std::max(image.info.size.height >> mip, 1u);
            const VkDeviceSize size = VkDeviceSize(w) * h * std::max(image.info.num_bits / 8, 1u);
            const VkBufferCreateInfo buffer_ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                               .size = size,
                                               .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
            const VmaAllocationCreateInfo alloc_ci{
                .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                         VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
                .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
            VkBuffer buffer{};
            VmaAllocation allocation{};
            VmaAllocationInfo info{};
            if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &buffer,
                                &allocation, &info) != VK_SUCCESS) {
                return;
            }
            runtime.Transit(&image, vk::ImageLayout::eTransferSrcOptimal,
                            vk::PipelineStageFlagBits2::eTransfer,
                            vk::AccessFlagBits2::eTransferRead);
            runtime.FlushBarriers();
            const vk::Image vk_image = image.GetImage();
            scheduler.Record([vk_image, buffer, w, h, mip](vk::CommandBuffer cmdbuf) {
                cmdbuf.copyImageToBuffer(
                    vk_image, vk::ImageLayout::eTransferSrcOptimal, vk::Buffer{buffer},
                    vk::BufferImageCopy{.imageSubresource = {vk::ImageAspectFlagBits::eColor, mip, 0, 1},
                                        .imageExtent = {w, h, 1}});
            });
            char path[512];
            const std::string mip_tag = mip ? std::format("_m{}", mip) : std::string{};
            std::snprintf(path, sizeof(path), "%s/%s%s_%03d_%llx_fmt%d_%ubit_%ux%u%s.raw",
                          base.c_str(), tag_prefix.c_str(), tag, dump_index,
                          (unsigned long long)image.info.guest_address,
                          int(image.info.pixel_format), image.info.num_bits, w, h, mip_tag.c_str());
            std::printf("ucndc: dump %s (flags %#x, mips %u, layers %u)\n", path,
                        unsigned(image.flags), image.info.resources.levels,
                        image.info.resources.layers);
            std::fflush(stdout);
            scheduler.DeferPriorityOperation([allocator = instance.GetAllocator(), buffer,
                                              allocation, info, size, file = std::string{path}] {
                vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
                if (FILE* f = std::fopen(file.c_str(), "wb")) {
                    std::fwrite(info.pMappedData, 1, size, f);
                    std::fclose(f);
                }
                vmaDestroyBuffer(allocator, buffer, allocation);
            });
          }
        };
        for (u32 i = 0; i < AmdGpu::NUM_COLOR_BUFFERS; ++i) {
            if (cb_descs[i].first) {
                char tag[16];
                std::snprintf(tag, sizeof(tag), "mrt%u", i);
                dump_image(texture_cache.GetImage(cb_descs[i].first), tag);
            }
        }
        // GOW3_DUMP_IMAGES=<hex addr>/<hex addr>/...: GPU images at these guest addresses.
        if (const char* list = image_list) {
            for (const char* p = list; p && *p;) {
                char* end = nullptr;
                const VAddr addr = std::strtoull(p, &end, 16);
                std::vector<VideoCore::ImageId> found;
                texture_cache.ForEachImageInRegion(
                    addr, 1, [&](VideoCore::ImageId id, VideoCore::Image&) { found.push_back(id); });
                std::printf("ucndc: dump images at %#llx: %zu\n", (unsigned long long)addr,
                            found.size());
                for (const auto id : found) {
                    dump_image(texture_cache.GetImage(id), "img");
                }
                p = end && *end == '/' ? end + 1 : nullptr;
            }
        }
        if (const char* mem = std::getenv("GOW3_DUMP_MEM")) {
            char* end = nullptr;
            const VAddr addr = std::strtoull(mem, &end, 16);
            const size_t len = end && *end == ':' ? std::strtoull(end + 1, nullptr, 16) : 0;
            char path[512];
            std::snprintf(path, sizeof(path), "%s/mem_%03d_%llx.raw", base.c_str(), dump_index,
                          (unsigned long long)addr);
            if (FILE* f = len ? std::fopen(path, "wb") : nullptr) {
                std::fwrite(reinterpret_cast<const void*>(addr), 1, len, f);
                std::fclose(f);
            }
        }
        begin_memo.valid = false;
        ++dump_index;
    }

}

// ucndc (GOW3_SAMPLER_LOG=<hex ps hash>, diagnosis, Opus): for draws with that pixel shader, the
// S# of each sampler (compare function, filters, clamps, border) with the VkCompareOp the port
// makes from it, and the images the texture cache holds at GOW3_SAMPLER_LOG_ADDR=<hex address>;
// also each draw that renders depth to that address, with its image id. 120 lines at most.
void Rasterizer::SamplerLog(const GraphicsPipeline* pipeline) {
    static const char* log_ps = std::getenv("GOW3_SAMPLER_LOG");
    if (!log_ps || !log_ps[0]) {
        return;
    }
    static const u64 ps_hash = std::strtoull(log_ps, nullptr, 16);
    static const VAddr watch = [] {
        const char* env = std::getenv("GOW3_SAMPLER_LOG_ADDR");
        return env && env[0] ? VAddr(std::strtoull(env, nullptr, 16)) : VAddr(0);
    }();
    static int lines = 0;
    static u32 last_depth_id = ~0u;
    if (lines >= 120) {
        return;
    }
    const auto* fs = pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
    const u64 hash = fs ? fs->pgm_hash : 0;
    if (watch && db_desc.first) {
        const auto& depth = texture_cache.GetImage(db_desc.first);
        if (depth.info.guest_address == watch && db_desc.first.index != last_depth_id) {
            last_depth_id = db_desc.first.index;
            std::printf("Sampler log: depth target at %#llx is image %u (%ux%u, fmt %d, %u bit), ps %016llx\n",
                        (unsigned long long)watch, db_desc.first.index, depth.info.size.width,
                        depth.info.size.height, int(depth.info.pixel_format), depth.info.num_bits,
                        (unsigned long long)hash);
            ++lines;
        }
    }
    if (!fs || hash != ps_hash) {
        return;
    }
    for (u32 i = 0; i < fs->samplers.size(); ++i) {
        const auto& sampler = fs->samplers[i];
        const auto ssharp = sampler.GetSharp(*fs);
        std::printf("Sampler log: ps %016llx sampler %u: raw %016llx %016llx, compare op %u -> Vk %s, "
                    "compareEnable %d, mag %u min %u mip %u, clamp %u/%u/%u, border %u, lod %u..%u, "
                    "valid %d\n",
                    (unsigned long long)hash, i, (unsigned long long)ssharp.raw0,
                    (unsigned long long)ssharp.raw1, u32(ssharp.depth_compare_func.Value()),
                    vk::to_string(LiverpoolToVK::DepthCompare(ssharp.depth_compare_func)).c_str(),
                    int(sampler.is_depth), u32(ssharp.xy_mag_filter.Value()),
                    u32(ssharp.xy_min_filter.Value()), u32(ssharp.mip_filter.Value()),
                    u32(ssharp.clamp_x.Value()), u32(ssharp.clamp_y.Value()),
                    u32(ssharp.clamp_z.Value()), u32(ssharp.border_color_type.Value()),
                    u32(ssharp.min_lod.Value()), u32(ssharp.max_lod.Value()), int(ssharp.Valid()));
        ++lines;
    }
    if (watch) {
        texture_cache.ForEachImageInRegion(watch, 1, [&](VideoCore::ImageId id, VideoCore::Image& image) {
            std::printf("Sampler log: ps %016llx: image %u at %#llx (%ux%u, fmt %d, %u bit, depth %d, "
                        "flags %#x)\n",
                        (unsigned long long)hash, id.index, (unsigned long long)image.info.guest_address,
                        image.info.size.width, image.info.size.height, int(image.info.pixel_format),
                        image.info.num_bits, int(image.info.props.is_depth), unsigned(image.flags));
            ++lines;
        });
    }
}

void Rasterizer::DrawIndirectRecord(const GraphicsPipeline* pipeline, bool is_indexed,
                                    const IndirectDraw& indirect) {
    if (DrawPipe::OnStageB()) {
        FrameCapture::Poll();
        scheduler.PopPendingOperations();
    }
    // uc3: who last modified the arguments, before this draw syncs them (frame analyzer).
    const bool capture_args = FrameCapture::Active();
    const bool args_cpu_modified = capture_args && buffer_cache.IsRegionCpuModified(indirect.args, 20);
    const bool args_gpu_modified = capture_args && buffer_cache.IsRegionGpuModified(indirect.args, 20);
    const VAddr count_address = indirect.count;
    const u32 stride = indirect.stride;
    const u32 max_count = indirect.max_count;

    // Indirect arguments may be GPU-written: leave these draws on camera fallback.
    motion_draw = false;
    motion_geometry = 0;
    PrepareRenderState(pipeline);
    if (upscaler->Enabled() && std::popcount(pipeline->GetGraphicsKey().mrt_mask) == 1) {
        const auto& viewport = Regs().viewports[0];
        upscaler->OnDraw(pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash,
                         cb_descs[0].first, db_desc.first,
                         UiComposition::NativeViewport(viewport.xscale * 2, viewport.yscale * 2));
    }
    if (!BindResources(pipeline)) {
        return;
    }
    const auto state = BeginRendering(pipeline);

    BindVertexBuffers(pipeline);
    if (is_indexed) {
        BindIndexBuffer(0, true);
    }

    const auto [buffer, base] =
        buffer_cache.ObtainBuffer(indirect.args, stride * max_count, false);
    needs_barrier |= runtime.IsBufferAccessed(buffer, base, stride * max_count);
    bound_buffers.emplace_back(buffer, base, stride * max_count,
                               vk::AccessFlagBits2::eIndirectCommandRead);

    const VideoCore::Buffer* count_buffer;
    u64 count_offset;
    if (count_address != 0) {
        std::tie(count_buffer, count_offset) = buffer_cache.ObtainBuffer(count_address, 4, false);
        needs_barrier |= runtime.IsBufferAccessed(count_buffer, count_offset, 4);
        bound_buffers.emplace_back(count_buffer, count_offset, 4,
                                   vk::AccessFlagBits2::eIndirectCommandRead);
    }

    if (needs_barrier) {
        runtime.FlushBarriers();
    }

    // gow3: screen-space (clip disabled) draws into the upscaler's output-size images.
    push_data.xscale *= target_scale[0];
    push_data.xoffset *= target_scale[0];
    push_data.yscale *= target_scale[1];
    push_data.yoffset *= target_scale[1];
    pipeline->BindResources(set_writes, push_data);
    draw_jitter = {};
    if (upscaler->Enabled() && db_desc.first && db_desc.first == camera_motion->Depth()) {
        draw_jitter = upscaler->Jitter();
    }
    UpdateDynamicState(pipeline, is_indexed);
    MarkPass(pipeline, state);
    scheduler.BeginRendering(state);
    if (FrameCapture::Active()) {
        // uc3: indirect draws in the frame analyzer (args written by the GPU or the CPU).
        const auto* ps = pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
        FrameCapture::Draw(pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash, ps ? ps->pgm_hash : 0, 0, 0);
        // The first argument record as guest memory holds it now (CPU view; a GPU-written value
        // shows only after a readback).
        u32 a[5] = {};
        std::memcpy(a, reinterpret_cast<const void*>(indirect.args), sizeof(a));
        char note[240];
        std::snprintf(note, sizeof(note),
                      "\n  indirect%s args %#llx stride %u max %u count %#llx; guest memory: %u %u %u %d %u; cpu modified %d, gpu modified %d",
                      is_indexed ? " indexed" : "", (unsigned long long)indirect.args, stride, max_count,
                      (unsigned long long)count_address, a[0], a[1], a[2], s32(a[3]), a[4],
                      int(args_cpu_modified), int(args_gpu_modified));
        FrameCapture::Note(note);
        // uc3: the blend state this indirect draw's pipeline was built with.
        if (const auto& bc = pipeline->GetGraphicsKey().blend_controls[0]; bc.enable) {
            std::snprintf(note, sizeof(note), "\n  blend ps %08x indirect: src %u dst %u func %u, alpha src %u dst %u func %u separate %u",
                          u32(ps ? ps->pgm_hash : 0), u32(bc.color_src_factor), u32(bc.color_dst_factor),
                          u32(bc.color_func), u32(bc.alpha_src_factor), u32(bc.alpha_dst_factor),
                          u32(bc.alpha_func), u32(bc.separate_alpha_blend));
        } else {
            std::snprintf(note, sizeof(note), "\n  blend ps %08x indirect: off", u32(ps ? ps->pgm_hash : 0));
        }
        FrameCapture::Note(note);
    }

    ASSERT(stride == (is_indexed ? sizeof(VkDrawIndexedIndirectCommand)
                                 : sizeof(VkDrawIndirectCommand)));
    const vk::Pipeline handle = pipeline->Handle();
    const vk::Pipeline square = SquarePass(*pipeline);
    const vk::Buffer args = buffer->Handle();
    const u64 args_offset = base;
    const vk::Buffer counts = count_address != 0 ? count_buffer->Handle() : vk::Buffer{};
    const u64 counts_offset = count_address != 0 ? count_offset : 0;
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        for (const vk::Pipeline bound : {handle, square}) {
            if (!bound) {
                break;
            }
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, bound);
            if (is_indexed) {
                if (counts) {
                    cmdbuf.drawIndexedIndirectCount(args, args_offset, counts, counts_offset, max_count,
                                                    stride);
                } else {
                    cmdbuf.drawIndexedIndirect(args, args_offset, max_count, stride);
                }
            } else if (counts) {
                cmdbuf.drawIndirectCount(args, args_offset, counts, counts_offset, max_count, stride);
            } else {
                cmdbuf.drawIndirect(args, args_offset, max_count, stride);
            }
        }
    });
    DebugState.IncDrawCall();
    DumpOnDraw(pipeline);
    SamplerLog(pipeline);

    ResetBindings(false);
    scheduler.KickRecording();
}

bool Rasterizer::OnComputeQueue() const {
    return liverpool && liverpool->CurrentQueueId() >= 0 &&
           static_cast<u32>(liverpool->CurrentQueueId()) != AmdGpu::Liverpool::GfxQueueId;
}

void Rasterizer::DispatchDirect(bool acb_undrained) {
    RENDERER_TRACE;
    StageBTimer a_timer{SbADispatch};
    Gow3Stats::dispatches.fetch_add(1, std::memory_order_relaxed);
    if (acb_undrained) {
        // uc3 (GOW3_HLE_INORDER): only a scalar copy runs without the drain.
        const ComputePipeline* copy_pipeline = pipeline_cache.GetComputePipeline();
        if (!copy_pipeline) {
            NoteInOrderReject(8);
        } else if (!IsGuestMemoryShaderHLE(
                       copy_pipeline->GetStage(Shader::SwStage::Compute).pgm_hash)) {
            static std::mutex seen_mutex;
            static std::unordered_map<u64, u32> seen;
            const auto& stage = copy_pipeline->GetStage(Shader::SwStage::Compute);
            {
                std::scoped_lock lock{seen_mutex};
                const u32 n = ++seen[stage.pgm_hash];
                if (n == 1 || n == 1000 || n == 100000) {
                    std::printf("uc3: in-order dispatch shader %llx: %u dispatches, groups %ux%ux%u, "
                                "%zu buffers, %zu images\n",
                                static_cast<unsigned long long>(stage.pgm_hash), n,
                                CsRegs().dim_x, CsRegs().dim_y, CsRegs().dim_z,
                                stage.buffers.size(), stage.images.size());
                }
            }
            NoteInOrderReject(7);
            // uc3 (GOW3_ACB_DISPATCH_PIPE=1, off by default, with GOW3_HLE_INORDER): other ACB
            // dispatches are handed over like draws too (UC2 rooftop: 8f067540 / 7d7e7206,
            // ~25k/s each). Waits, AcquireMem and DMA on the ACB still drain.
            static const bool acb_dispatch_pipe = [] {
                const char* env = std::getenv("GOW3_ACB_DISPATCH_PIPE");
                return env && env[0] == '1';
            }();
            if (acb_dispatch_pipe) {
                acb_undrained = false;
            }
        } else if (ExecuteScalarCopyInOrder(copy_pipeline->GetStage(Shader::SwStage::Compute),
                                            CsRegs(), *this)) {
            return;
        }
        if (acb_undrained) {
            DrainDrawPipe(DrawPipe::ReasonCompute);
        }
    }
    // gow3: like draws, handed to the draw recording thread after the pipeline selection.
    const bool pipelined = UseDrawPipe() && !Gow3Toggle::Disabled(Gow3Toggle::PipelinedDispatch);
    if (!pipelined) {
        DrainDrawPipe();
    }
    StageBTimer pipe_timer{SbACsPipe};
    const ComputePipeline* pipeline = pipeline_cache.GetComputePipeline();
    pipe_timer.Stop();
    if (!pipeline) {
        return;
    }
    StageBTimer hle_timer{SbAdHle};
    const bool guest_hle =
        IsGuestMemoryShaderHLE(pipeline->GetStage(Shader::SwStage::Compute).pgm_hash);
    hle_timer.Stop();
    if (Gow3Stats::b_census) {
        NoteSharpCensus(pipeline->GetStage(Shader::SwStage::Compute), *this);
    }
    if (pipelined && !guest_hle) {
        PostDraw(pipeline, nullptr, false, 0, &CsRegs());
        return;
    }
    if (pipelined) {
        // uc3 A4.1c (GOW3_HLE_BIG_PIPE=1, off by default): a copy of 1 MB or more (the scene
        // depth) ends as a GPU copy after SyncImageToBuffer, writing no guest memory from the
        // CPU: hand it over like any dispatch instead of draining (~47/s, ~3.4 ms each in the
        // Cidadela). The recording thread runs the HLE in order, as owner of the caches.
        static const bool big_pipe = [] {
            const char* env = std::getenv("GOW3_HLE_BIG_PIPE");
            return env && env[0] == '1';
        }();
        unsigned long long big_dst = 0;
        const unsigned long long big_size =
            big_pipe ? ScalarCopyBytes(pipeline->GetStage(Shader::SwStage::Compute), CsRegs(), &big_dst)
                     : 0;
        if (big_pipe && big_dst && big_size >= BigPipeMin()) {
            // As the in-order path: the destination is pending until the recording thread runs
            // the copy (it may end as a guest copy), so a later draw does not read it stale.
            NotePendingGpuWrite(big_dst, big_size);
            NoteHleInOrderWrite(big_dst, big_size);
            Gow3Stats::big_pipe_hist[Gow3Stats::SizeBucket(big_size)].fetch_add(
                1, std::memory_order_relaxed);
            static std::atomic<u32> handed{0};
            const u32 n = handed.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 2 || (n & 4095) == 0) {
                std::printf("uc3: big scalar copies handed to the recording thread: %u\n", n);
            }
            PostDraw(pipeline, nullptr, false, 0, &CsRegs());
            return;
        }
        // uc3 (GOW3_HLE_INORDER_GFX=1, off by default): a scalar copy on the graphics queue
        // snapshots its source here and writes the destination on the recording thread, in
        // order, instead of draining it (~7% of the command processor in the castle).
        static const bool inorder_gfx = [] {
            const char* env = std::getenv("GOW3_HLE_INORDER_GFX");
            return env && env[0] == '1';
        }();
        if (inorder_gfx &&
            ExecuteScalarCopyInOrder(pipeline->GetStage(Shader::SwStage::Compute), CsRegs(), *this)) {
            return;
        }
        // uc3: an HLE that writes guest memory runs here, after the draws before it.
        DrainDrawPipe();
    }
    DispatchRecord(pipeline);
}

void Rasterizer::DispatchRecord(const ComputePipeline* pipeline) {
    FrameCapture::Poll();
    gbuffer_draw = false;

    scheduler.PopPendingOperations();

    const auto& cs_program = CsRegs();

    const auto& cs = pipeline->GetStage(Shader::SwStage::Compute);
    // After the resource binding: its transfers (image uploads, downloads) mark themselves.
    const auto mark = [&] {
        if (auto* profiler = GpuProfiler::Get()) {
            profiler->Mark(cs.pgm_hash ^ 0xD15Aull, [&] {
                return fmt::format("dispatch cs {:016x} ({}x{}x{} groups)", cs.pgm_hash,
                                   cs_program.dim_x, cs_program.dim_y, cs_program.dim_z);
            });
        }
    };
    if (upscaler->Enabled()) {
        upscaler->OnDispatch(cs.pgm_hash);
    }
    {
        StageBTimer timer{SbDispHle};
        if (ExecuteShaderHLE(cs, Regs(), cs_program, *this)) {
            return;
        }
    }

    {
        StageBTimer timer{SbDispBind};
        if (!BindResources(pipeline)) {
            return;
        }
    }
    StageBTimer record_timer{SbDispRecord};

    // uc3: marked before the barrier, so its wait is charged to this dispatch, not the previous
    // label (Fable R.3).
    mark();
    if (needs_barrier) {
        runtime.FlushBarriers();
    }

    scheduler.EndRendering();
    pipeline->BindResources(set_writes, push_data);

    const vk::Pipeline handle = pipeline->Handle();
    const u32 dim_x = cs_program.dim_x, dim_y = cs_program.dim_y, dim_z = cs_program.dim_z;
    if (FrameCapture::Active()) {
        FrameCapture::Dispatch(cs.pgm_hash, dim_x, dim_y, dim_z);
    }
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, handle);
        cmdbuf.dispatch(dim_x, dim_y, dim_z);
    });
    DebugState.IncDispatch();

    ResetBindings(true);
    scheduler.KickRecording();
}

void Rasterizer::DispatchIndirect(VAddr address, u32 offset, u32 size) {
    RENDERER_TRACE;
    // gow3: handed to the recording thread like direct dispatches.
    const bool pipelined = UseDrawPipe() && !Gow3Toggle::Disabled(Gow3Toggle::PipelinedIndirectDraws);
    if (!pipelined) {
        DrainDrawPipe();
    }
    const ComputePipeline* pipeline = pipeline_cache.GetComputePipeline();
    if (!pipeline) {
        return;
    }
    if (pipelined) {
        const IndirectDraw indirect = {address + offset, 0, size, 1};
        PostDraw(pipeline, nullptr, false, 0, &CsRegs(), &indirect);
        return;
    }
    DispatchIndirectRecord(pipeline, address + offset, size);
}

void Rasterizer::DispatchIndirectRecord(const ComputePipeline* pipeline, VAddr address,
                                        u32 size) {
    scheduler.PopPendingOperations();
    const u32 offset = 0;

    if (!BindResources(pipeline)) {
        return;
    }

    const auto [buffer, base] = buffer_cache.ObtainBuffer(address + offset, size, false);
    needs_barrier |= runtime.IsBufferAccessed(buffer, base, size);
    bound_buffers.emplace_back(buffer, base, size, vk::AccessFlagBits2::eIndirectCommandRead);

    if (needs_barrier) {
        runtime.FlushBarriers();
    }

    scheduler.EndRendering();
    pipeline->BindResources(set_writes, push_data);

    const vk::Pipeline handle = pipeline->Handle();
    const vk::Buffer args = buffer->Handle();
    const u64 args_offset = base;
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, handle);
        cmdbuf.dispatchIndirect(args, args_offset);
    });
    if (FrameCapture::Active()) {
        // uc3: indirect dispatches in the frame analyzer, with the group counts guest memory
        // holds now (GPU-written ones show only after a readback).
        u32 g[3] = {};
        std::memcpy(g, reinterpret_cast<const void*>(address), sizeof(g));
        FrameCapture::Dispatch(pipeline->GetStage(Shader::SwStage::Compute).pgm_hash, g[0], g[1], g[2]);
        char note[128];
        std::snprintf(note, sizeof(note), "\n  indirect dispatch args %#llx", (unsigned long long)address);
        FrameCapture::Note(note);
    }
    DebugState.IncDispatch();

    ResetBindings(true);
    scheduler.KickRecording();
}

u64 Rasterizer::Flush() {
    DrainDrawPipe();
    const u64 current_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return current_tick;
}

void Rasterizer::Finish() {
    DrainDrawPipe();
    scheduler.Finish();
}

void Rasterizer::OnSubmit() {
    DrainDrawPipe();
    Gow3Stats::frame_ticks.fetch_add(1, std::memory_order_relaxed);
    buffer_cache.TickFrame();
    buffer_cache.CompleteLeafDownloads();
    texture_cache.ProcessDownloadImages();
    texture_cache.RunGarbageCollector();
    runtime.TickFrame();
}

const PreparedStage* Rasterizer::FindPreparedStage(const Shader::Info& stage) const {
    // Sharps a draw-preparation worker read from the same flattened user data.
    if (!bind_prepared || Gow3Toggle::Disabled(Gow3Toggle::PreparedResources)) {
        return nullptr;
    }
    for (u32 i = 0; i < bind_prepared->num_stages; ++i) {
        const auto& candidate = bind_prepared->stages[i];
        if (&candidate.program->info == &stage &&
            candidate.num_images == stage.images.size() &&
            candidate.num_samplers == stage.samplers.size() &&
            candidate.num_buffers == stage.buffers.size()) {
            return &candidate;
        }
    }
    return nullptr;
}

bool Rasterizer::BindHelperWanted() {
    // A spinning helper pays off only with cores to spare (the recording thread spins too).
    const char* env = std::getenv("GOW3_TEXTURE_HELPER");
    if (env && env[0]) {
        return env[0] == '1';
    }
    // Measured neutral on 16 threads (docs/parallel_gpu.md): opt-in.
    return false;
}

bool Rasterizer::HelperEligible(const Pipeline* pipeline) const {
    if (pipeline->IsCompute() || !bind_helper.Available() || FrameCapture::Active() ||
        Gow3Toggle::Disabled(Gow3Toggle::TextureBindHelper) ||
        Gow3Toggle::Disabled(Gow3Toggle::TextureBindingMemo) ||
        Gow3Toggle::Disabled(Gow3Toggle::UpdateImageFastPath)) {
        return false;
    }
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        // Mip arrays make the descriptor layout depend on the T#s.
        for (const auto& image : stage->images) {
            if (image.mip_fallback_mode != Shader::MipStorageFallbackMode::None) {
                return false;
            }
        }
        // Storage writes invalidate cached images from the buffer side.
        for (const auto& buffer : stage->buffers) {
            if (!buffer.IsSpecial() && buffer.is_written) {
                return false;
            }
        }
    }
    return true;
}

// uc3: GOW3_UC3_MOVIE_FIX (see Shader::Uc3MovieFixKey). The movie NV12->RGB compute reads the
// decoded 1920x1080 picture through two of the game's screen-sized render targets (luma R8 and
// chroma R8G8, chroma at luma + W * align(H, 64)). Above 1080p their T#s are pointed back at the
// picture as the decoder lays it out: luma 1920x1080 with pitch 1920, chroma 960x540 at +1920*1088.
static bool Uc3MovieSharp(const Shader::Info& stage, u32 image_index, AmdGpu::Image& tsharp) {
    if (stage.pgm_hash != Shader::Uc3MovieShaderHash || image_index > 1 ||
        !Shader::Uc3MovieFixKey() || tsharp.Address() == 0) {
        return false;
    }
    const u32 scale = image_index == 1 ? 2u : 1u;
    const u32 luma_w = u32(tsharp.width + 1) * scale, luma_h = u32(tsharp.height + 1) * scale;
    if (luma_w == 1920 && luma_h == 1080) {
        return false;
    }
    VAddr luma = tsharp.Address();
    if (image_index == 1) {
        luma -= u64(luma_w) * ((luma_h + 63u) & ~63u);
    }
    const u32 w = 1920 / scale, h = 1080 / scale;
    const VAddr base = image_index == 1 ? luma + 1920u * 1088u : luma;
    tsharp.base_address = base >> 8;
    tsharp.width = w - 1;
    tsharp.height = h - 1;
    tsharp.pitch = w - 1;
    return true;
}

bool Rasterizer::TexturesBindableOnHelper(const Shader::Info& stage,
                                          const PreparedStage* prepared) {
    const u64 generation = texture_cache.RegistryGeneration();
    for (u32 image_index = 0; image_index < stage.images.size(); ++image_index) {
        const auto& image_desc = stage.images[image_index];
        if (image_desc.is_written) {
            return false;
        }
        bool movie_sharp = false;
        const auto tsharp = [&] {
            auto sharp = prepared ? prepared->image_sharps[image_index] : image_desc.GetSharp(stage);
            movie_sharp = Uc3MovieSharp(stage, image_index, sharp);
            return sharp;
        }();
        const auto data_fmt = tsharp.GetDataFmt();
        const auto num_fmt = tsharp.GetNumberFmt();
        if (tsharp.Address() == 0 || data_fmt == AmdGpu::DataFormat::FormatInvalid ||
            !memory->IsValidGpuMapping(tsharp.Address(), 0) ||
            !IsKnownFormat(data_fmt, num_fmt)) {
            continue; // null descriptor
        }
        const auto& entry =
            prepared && !movie_sharp
                ? CachedImageDescEntry(tsharp, image_desc, prepared->image_hashes[image_index])
                : CachedImageDescEntry(tsharp, image_desc);
        if (entry.found_generation != generation) {
            return false;
        }
        VideoCore::ImageId image_id = entry.found_id;
        auto* image = &texture_cache.GetImage(image_id);
        if (const auto depth_image_id =
                texture_cache.GetAssociatedDepth(*image, entry.found_desc.view_info.format)) {
            image_id = depth_image_id;
            image = &texture_cache.GetImage(image_id);
        }
        if (image->binding.needs_rebind || !texture_cache.IsUpToDate(image_id) ||
            scene_targets->Tracks(image->image_uid) ||
            (upscaler->Enabled() && upscaler->RedirectsSampled(image_id))) {
            return false;
        }
    }
    return true;
}

void Rasterizer::RunTextureTask(void* context) {
    auto& self = *static_cast<Rasterizer*>(context);
    auto& task = self.texture_task;
    for (u32 i = 0; i < task.count; ++i) {
        auto& stage = task.stages[i];
        if (!self.TexturesBindableOnHelper(*stage.info, stage.prepared)) {
            return;
        }
        auto binding = stage.binding;
        u32 write_index = stage.write_index;
        self.BindTextures(*stage.info, stage.prepared, binding, write_index, task.barrier, true);
        task.completed = i + 1;
    }
}

void Rasterizer::JoinBindHelper(void* context) {
    if (!BindHelper::OnHelper()) {
        static_cast<Rasterizer*>(context)->bind_helper.Join();
    }
}

void Rasterizer::OnFence() {
    ProcessDownloadImages();
}

bool Rasterizer::BindResources(const Pipeline* pipeline) {
    if (IsComputeImageCopy(pipeline) || IsComputeMetaClear(pipeline) ||
        IsComputeImageClear(pipeline)) {
        return false;
    }

    set_write_index = 0;
    set_writes.clear();
    buffer_infos.clear();
    image_infos.clear();
    // gow3: G-buffer passes sample material textures for a scene rendered below the output
    // size; the temporal upscaler restores the detail of the output's mip level (FSR guide:
    // log2(render / output)). Shadows, post-processing and UI keep the guest's bias.
    sampler_lod_bias = 0.0f;
    if (!pipeline->IsCompute() && !Gow3Toggle::Disabled(Gow3Toggle::SceneMipBias) &&
        std::popcount(static_cast<const GraphicsPipeline*>(pipeline)->GetGraphicsKey().mrt_mask &
                      0xff) >= 5) {
        sampler_lod_bias = upscaler->SceneMipBias();
        static float reported = 0.0f;
        if (sampler_lod_bias != reported) {
            reported = sampler_lod_bias;
            std::printf("Upscaler: scene texture LOD bias %.2f\n", sampler_lod_bias);
        }
    }

    bool uses_dma = false;

    static const bool stats = std::getenv("GOW3_FRAME_STATS") != nullptr;
    if (stats && ((helper_full + helper_partial + helper_serial) & 1023) == 0) {
        static auto window = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - window >= std::chrono::seconds(5)) {
            window = now;
            const double forks = std::max<double>(1, helper_full + helper_partial);
            std::printf("Texture helper: %llu binds in parallel, %llu partly, %llu serial; "
                        "per fork %.0f cycles helper task, %.0f cycles GPU thread wait\n",
                        static_cast<unsigned long long>(helper_full),
                        static_cast<unsigned long long>(helper_partial),
                        static_cast<unsigned long long>(helper_serial),
                        bind_helper.task_cycles.exchange(0) / forks,
                        bind_helper.wait_cycles / forks);
            bind_helper.wait_cycles = 0;
            helper_full = helper_partial = helper_serial = 0;
        }
    }

    // Bind resource buffers and textures.
    Shader::Backend::Bindings binding{};
    push_data = MakeUserData(Regs());
    if (!HelperEligible(pipeline)) {
        ++helper_serial;
        for (const auto* stage : pipeline->GetStages()) {
            if (!stage) {
                continue;
            }
            set_writes.resize(set_writes.size() + stage->buffers.size() + stage->images.size() +
                              stage->samplers.size());
            const PreparedStage* prepared = FindPreparedStage(*stage);
            {
                StageBTimer timer{SbBindBuffers};
                BindBuffers(*stage, prepared, binding, push_data, set_write_index);
            }
            {
                StageBTimer timer{SbBindTextures};
                BindTextures(*stage, prepared, binding, set_write_index, needs_barrier);
            }
            uses_dma |= stage->uses_dma;
        }
    } else {
        // gow3: textures on the helper, buffers here. Without mip arrays every stage's
        // descriptors are [buffers][images][samplers], one each, so both sides know their
        // binding numbers and descriptor write slots up front.
        struct BufferStage {
            const Shader::Info* info;
            const PreparedStage* prepared;
            Shader::Backend::Bindings binding;
            u32 write_index;
        };
        std::array<BufferStage, Shader::MaxStageTypes> buffer_stages;
        auto& task = texture_task;
        task.count = 0;
        task.completed = 0;
        task.barrier = false;
        for (const auto* stage : pipeline->GetStages()) {
            if (!stage) {
                continue;
            }
            const u32 num_buffers = static_cast<u32>(stage->buffers.size());
            const u32 num_descriptors =
                num_buffers + static_cast<u32>(stage->images.size() + stage->samplers.size());
            set_writes.resize(set_writes.size() + num_descriptors);
            const PreparedStage* prepared = FindPreparedStage(*stage);
            buffer_stages[task.count] = {stage, prepared, binding, set_write_index};
            auto texture_binding = binding;
            texture_binding.buffer += num_buffers;
            texture_binding.unified += num_buffers;
            task.stages[task.count] = {stage, prepared, texture_binding,
                                       set_write_index + num_buffers};
            ++task.count;
            binding.buffer += num_buffers;
            binding.unified += num_descriptors;
            set_write_index += num_descriptors;
            uses_dma |= stage->uses_dma;
        }
        bind_helper.Fork(&RunTextureTask, this);
        for (u32 i = 0; i < task.count; ++i) {
            auto& stage = buffer_stages[i];
            BindBuffers(*stage.info, stage.prepared, stage.binding, push_data, stage.write_index);
        }
        if (draw_inputs.pending && !Gow3Toggle::Disabled(Gow3Toggle::EarlyDrawInputs)) {
            ResolveVertexBuffers(draw_inputs.pipeline, draw_inputs.prepared);
            if (draw_inputs.is_indexed) {
                ResolveIndexBuffer(draw_inputs.index_offset);
            }
            draw_inputs.resolved = true;
        }
        bind_helper.Join();
        needs_barrier |= task.barrier;
        ++(task.completed == task.count ? helper_full
                                        : task.completed ? helper_partial : helper_serial);
        // Stages the helper left (an image needing work that records commands).
        for (u32 i = task.completed; i < task.count; ++i) {
            auto& stage = task.stages[i];
            u32 write_index = stage.write_index;
            BindTextures(*stage.info, stage.prepared, stage.binding, write_index, needs_barrier);
        }
    }

    if (uses_dma) {
        StageBTimer timer{SbBindDma};
        buffer_cache.SynchronizeDmaBuffers();
    }

    return true;
}

void Rasterizer::BindVertexBuffers(const GraphicsPipeline* pipeline, const PreparedDraw* prepared) {
    ResolveVertexBuffers(pipeline, prepared);
    EmitVertexBuffers();
}

void Rasterizer::EmitVertexBuffers() {
    // gow3: only the used entries go into the recording chunk (the static vectors hold 32 of
    // each: ~3 KiB copied per draw).
    auto& v = vertex_binds;
    const bool dynamic_input = instance.IsVertexInputDynamicState();
    const u32 num_buffers = v.num_buffers;
    scheduler.ReserveRecordData(
        v.bindings.size() * sizeof(v.bindings[0]) + v.attributes.size() * sizeof(v.attributes[0]) +
        num_buffers * (sizeof(vk::Buffer) + 3 * sizeof(vk::DeviceSize)) + 128);
    if (dynamic_input) {
        scheduler.Record([bindings = scheduler.RecordData(std::span<const vk::VertexInputBindingDescription2EXT>{v.bindings.data(), v.bindings.size()}),
                          attributes = scheduler.RecordData(std::span<const vk::VertexInputAttributeDescription2EXT>{
                              v.attributes.data(), v.attributes.size()})](
                             vk::CommandBuffer cmdbuf) {
            cmdbuf.setVertexInputEXT(bindings, attributes);
        });
    }
    if (num_buffers == 0) {
        return;
    }
    const auto buffers = scheduler.RecordData(std::span<const vk::Buffer>{v.host_buffers.data(), num_buffers});
    const auto offsets = scheduler.RecordData(std::span<const vk::DeviceSize>{v.host_offsets.data(), num_buffers});
    if (dynamic_input) {
        scheduler.Record([num_buffers, buffers, offsets](vk::CommandBuffer cmdbuf) {
            cmdbuf.bindVertexBuffers(0, num_buffers, buffers.data(), offsets.data());
        });
        return;
    }
    const auto sizes = scheduler.RecordData(std::span<const vk::DeviceSize>{v.host_sizes.data(), num_buffers});
    const auto strides = scheduler.RecordData(std::span<const vk::DeviceSize>{v.host_strides.data(), num_buffers});
    scheduler.Record([num_buffers, buffers, offsets, sizes, strides](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindVertexBuffers2(0, num_buffers, buffers.data(), offsets.data(), sizes.data(),
                                  strides.data());
    });
}

void Rasterizer::ResolveVertexBuffers(const GraphicsPipeline* pipeline,
                                      const PreparedDraw* prepared) {
    const auto& regs = Regs();
    auto& v = vertex_binds;
    v.num_buffers = 0;
    v.host_buffers.clear();
    v.host_offsets.clear();
    v.host_sizes.clear();
    v.host_strides.clear();
    auto& attributes = v.attributes;
    auto& bindings = v.bindings;
    attributes.clear();
    bindings.clear();
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT> divisors;
    VertexInputs<AmdGpu::Buffer> guest_buffers;
    // Inputs a draw-preparation worker read from the same registers and (verified) user data.
    const auto& fetch = pipeline->GetFetchShader();
    const PreparedVertexInputs* ready =
        prepared && prepared->vertex.valid && instance.IsVertexInputDynamicState() && fetch &&
                fetch->attributes.size() == prepared->vertex.count && !FrameCapture::Active() &&
                !Gow3Toggle::Disabled(Gow3Toggle::PreparedResources)
            ? &prepared->vertex
            : nullptr;
    if (ready) {
        attributes.assign(ready->attributes, ready->attributes + ready->count);
        bindings.assign(ready->bindings, ready->bindings + ready->count);
        guest_buffers.assign(ready->buffers, ready->buffers + ready->count);
    } else {
        pipeline->GetVertexInputs(attributes, bindings, divisors, guest_buffers,
                                  regs.vgt_instance_step_rate_0, regs.vgt_instance_step_rate_1);
    }

    if (bindings.empty()) {
        // If there are no bindings, there is nothing further to do.
        return;
    }
    if (motion_draw && !guest_buffers.empty()) {
        // Include every stream: identical position buffers can be paired with different
        // skinning/instance data. Index topology and base offsets are matched separately.
        motion_geometry = ready ? ready->buffers_hash
                                : XXH3_64bits(guest_buffers.data(),
                                              guest_buffers.size() * sizeof(AmdGpu::Buffer));
    }
    // Object motion research: vertex streams of G-buffer draws.
    if (FrameCapture::Active() && gbuffer_draw) {
        for (const auto& vb : guest_buffers) {
            char note[128];
            std::snprintf(note, sizeof(note), "\n  gb vb at %#llx size %u stride %u",
                          (unsigned long long)vb.base_address, u32(vb.GetSize()),
                          u32(vb.GetStride()));
            FrameCapture::Note(note);
        }
    }

    struct BufferRange {
        VAddr base_address;
        VAddr end_address;
        const VideoCore::Buffer* buffer;
        u64 offset;

        [[nodiscard]] size_t GetSize() const {
            return end_address - base_address;
        }
    };

    // Build list of ranges covering the requested buffers
    VertexInputs<BufferRange> ranges{};
    VertexInputs<BufferRange> ranges_merged{};
    if (ready) {
        for (u32 i = 0; i < ready->num_ranges; ++i) {
            ranges_merged.emplace_back(ready->ranges[i].base, ready->ranges[i].end);
        }
    } else {
        for (const auto& buffer : guest_buffers) {
            if (buffer.base_address != 0 && buffer.GetSize() > 0) {
                ranges.emplace_back(buffer.base_address, buffer.base_address + buffer.GetSize());
            }
        }
    }

    // Merge connecting ranges together
    if (!ranges.empty()) {
        std::ranges::sort(ranges, [](const BufferRange& lhv, const BufferRange& rhv) {
            return lhv.base_address < rhv.base_address;
        });
        ranges_merged.emplace_back(ranges[0]);
        for (auto range : ranges) {
            auto& prev_range = ranges_merged.back();
            if (prev_range.end_address < range.base_address) {
                ranges_merged.emplace_back(range);
            } else {
                prev_range.end_address = std::max(prev_range.end_address, range.end_address);
            }
        }
    }

    // Map buffers for merged ranges
    for (auto& range : ranges_merged) {
        const u64 size = memory->ClampRangeSize(range.base_address, range.GetSize());
        std::tie(range.buffer, range.offset) =
            buffer_cache.ObtainBuffer(range.base_address, size, false);
        needs_barrier |= runtime.IsBufferAccessed(range.buffer, range.offset, size);
        bound_buffers.emplace_back(range.buffer, range.offset, size,
                                   vk::AccessFlagBits2::eVertexAttributeRead);
    }

    // Bind vertex buffers
    auto& host_buffers = v.host_buffers;
    auto& host_offsets = v.host_offsets;
    auto& host_sizes = v.host_sizes;
    auto& host_strides = v.host_strides;
    u32 stream_with_memory = 0;
    for (const auto& buffer : guest_buffers) {
        if (buffer.base_address != 0 && buffer.GetSize() > 0) {
            const auto host_buffer_info =
                ready ? ranges_merged.begin() + ready->range_index[stream_with_memory++]
                      : std::ranges::find_if(ranges_merged, [&](const BufferRange& range) {
                            return buffer.base_address >= range.base_address &&
                                   buffer.base_address < range.end_address;
                        });
            ASSERT(host_buffer_info != ranges_merged.cend());
            host_buffers.emplace_back(host_buffer_info->buffer->Handle());
            host_offsets.push_back(host_buffer_info->offset + buffer.base_address -
                                   host_buffer_info->base_address);
        } else {
            host_buffers.emplace_back(VK_NULL_HANDLE);
            host_offsets.push_back(0);
        }
        host_sizes.push_back(buffer.GetSize());
        host_strides.push_back(buffer.GetStride());
    }

    v.num_buffers = static_cast<u32>(guest_buffers.size());
}

void Rasterizer::BindIndexBuffer(u32 index_offset, bool is_indirect) {
    ResolveIndexBuffer(index_offset, is_indirect);
    EmitIndexBuffer();
}

void Rasterizer::EmitIndexBuffer() {
    scheduler.Record([handle = index_bind.handle, offset = index_bind.offset,
                      size = index_bind.size, type = index_bind.type,
                      bind2 = instance.IsMaintenance5Supported()](vk::CommandBuffer cmdbuf) {
        if (bind2) {
            cmdbuf.bindIndexBuffer2KHR(handle, offset, size, type);
        } else {
            cmdbuf.bindIndexBuffer(handle, offset, type);
        }
    });
}

void Rasterizer::ResolveIndexBuffer(u32 index_offset, bool is_indirect) {
    const auto& regs = Regs();

    // Figure out index type and size.
    const bool is_index16 = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16;
    const vk::IndexType index_type = is_index16 ? vk::IndexType::eUint16 : vk::IndexType::eUint32;
    const u32 index_size = is_index16 ? sizeof(u16) : sizeof(u32);
    const VAddr base_address = regs.index_base_address.Address<VAddr>();
    const VAddr index_address = base_address + u64(index_offset) * index_size;

    // Bind index buffer.
    // Upstream #5278/#5327: the draw's count clamped to the buffer's capacity (indirect
    // draws read the whole capacity) and to the mapping; a null buffer when nothing is mapped.
    // ucndc (GOW3_INDIRECT_IB_FIX=1): indirect draws see at least IndexBufferSize's count
    // (SuperMedo binds the whole buffer). The first 30 indirect bindings are always reported.
    static const bool ib_fix = [] {
        const char* env = std::getenv("GOW3_INDIRECT_IB_FIX");
        return env && env[0] == '1';
    }();
    const u32 num_indices =
        is_indirect ? (ib_fix ? std::max(regs.num_indices, regs.max_index_size)
                              : regs.max_index_size)
                    : std::min(regs.num_indices, regs.max_index_size);
    if (is_indirect) {
        static std::atomic<u32> reported{0};
        if (reported.fetch_add(1, std::memory_order_relaxed) < 30) {
            std::fprintf(stderr,
                         "ucndc: indirect index buffer %#llx: max_index_size %u, num_indices %u, "
                         "%s, binding %u indices\n",
                         static_cast<unsigned long long>(base_address), regs.max_index_size,
                         regs.num_indices, is_index16 ? "16-bit" : "32-bit", num_indices);
        }
    }
    const u64 requested_size = u64(num_indices) * index_size;
    const u64 mapped_size =
        base_address != 0 ? memory->ClampRangeSize(index_address, requested_size) : 0;
    u32 index_buffer_size = static_cast<u32>(Common::AlignDown(mapped_size, index_size));
    index_bind = {VK_NULL_HANDLE, 0, 0, index_type};
    if (index_buffer_size != 0) {
        const auto [buffer, offset] =
            buffer_cache.ObtainBuffer(index_address, index_buffer_size, false);
        needs_barrier |= runtime.IsBufferAccessed(buffer, offset, index_buffer_size);
        bound_buffers.emplace_back(buffer, offset, index_buffer_size,
                                   vk::AccessFlagBits2::eIndexRead);
        index_bind = {buffer->Handle(), offset, index_buffer_size, index_type};
    } else if (!instance.IsMaintenance6Supported()) {
        const auto* null_buffer = buffer_cache.GetNullIndexBuffer();
        index_buffer_size = static_cast<u32>(null_buffer->SizeBytes());
        needs_barrier |= runtime.IsBufferAccessed(null_buffer, 0, index_buffer_size);
        bound_buffers.emplace_back(null_buffer, 0, index_buffer_size,
                                   vk::AccessFlagBits2::eIndexRead);
        index_bind = {null_buffer->Handle(), 0, index_buffer_size, index_type};
    }
}

void Rasterizer::ResetBindings(bool is_compute) {
    for (auto& image_id : bound_images) {
        texture_cache.GetImage(image_id).binding = {};
    }
    for (const auto [buffer, offset, size, src_access] : bound_buffers) {
        const auto dst_stage = is_compute ? vk::PipelineStageFlagBits2::eComputeShader
                                          : vk::PipelineStageFlagBits2::eAllGraphics;
        runtime.AccessBuffer(buffer, offset, size, dst_stage, src_access);
    }
    bound_images.clear();
    bound_buffers.clear();
    needs_barrier = false;
}

bool Rasterizer::IsComputeMetaClear(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Most of the time when a metadata is updated with a shader it gets cleared. It means
    // we can skip the whole dispatch and update the tracked state instead. Also, it is not
    // intended to be consumed and in such rare cases (e.g. HTile introspection, CRAA) we
    // will need its full emulation anyways.
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);

    // Assume if a shader reads metadata, it is a copy shader.
    for (const auto& desc : info.buffers) {
        const VAddr address = desc.GetSharp(info).base_address;
        if (!desc.IsSpecial() && !desc.is_written && texture_cache.IsMeta(address)) {
            return false;
        }
    }

    // Metadata surfaces are tiled and thus need address calculation to be written properly.
    // If a shader wants to encode HTILE, for example, from a depth image it will have to compute
    // proper tile address from dispatch invocation id. This address calculation contains an xor
    // operation so use it as a heuristic for metadata writes that are probably not clears.
    if (!info.has_bitwise_xor) {
        // Assume if a shader writes metadata without address calculation, it is a clear shader.
        for (const auto& desc : info.buffers) {
            const VAddr address = desc.GetSharp(info).base_address;
            if (!desc.IsSpecial() && desc.is_written && texture_cache.ClearMeta(address)) {
                // Assume all slices were updates
                LOG_TRACE(Render_Vulkan, "Metadata update skipped");
                return true;
            }
        }
    }
    return false;
}

bool Rasterizer::IsComputeImageCopy(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Ensure shader only has 2 bound buffers
    const auto& cs_pgm = CsRegs();
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);
    if (cs_pgm.num_thread_x.full != 64 || info.buffers.size() < 2 || info.buffers.size() > 3 ||
        !info.images.empty()) {
        return false;
    }

    // Those 2 buffers must both be formatted. One must be source and another destination.
    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (!desc0.is_formatted || !desc1.is_formatted || desc0.is_written == desc1.is_written) {
        return false;
    }
    if (info.buffers.size() == 3 && info.buffers[2].buffer_type != Shader::BufferType::Flatbuf) {
        return false;
    }

    // Buffers must have the same size and each thread of the dispatch must copy 1 dword of data
    const AmdGpu::Buffer buf0 = desc0.GetSharp(info);
    const AmdGpu::Buffer buf1 = desc1.GetSharp(info);
    if (buf0.GetSize() != buf1.GetSize() || cs_pgm.dim_x != (buf0.GetSize() / 256)) {
        return false;
    }

    // Find images the buffer alias
    const auto image0_id = texture_cache.FindImageFromRange(buf0.base_address, buf0.GetSize());
    if (!image0_id) {
        return false;
    }
    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        return false;
    }

    // Image copy must be valid
    VideoCore::Image& image0 = texture_cache.GetImage(image0_id);
    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    if (image0.info.guest_size != image1.info.guest_size ||
        image0.info.pitch != image1.info.pitch || image0.info.guest_size != buf0.GetSize() ||
        image0.info.num_bits != image1.info.num_bits) {
        return false;
    }

    // Perform image copy
    VideoCore::Image& src_image = desc0.is_written ? image1 : image0;
    VideoCore::Image& dst_image = desc0.is_written ? image0 : image1;
    runtime.CopyColorAndDepth(&src_image, &dst_image);
    return true;
}

bool Rasterizer::IsComputeImageClear(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Ensure shader only has 2 bound buffers
    const auto& cs_pgm = CsRegs();
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);
    if (cs_pgm.num_thread_x.full != 64 || info.buffers.size() < 2 || info.buffers.size() > 3 ||
        !info.images.empty()) {
        return false;
    }

    // From those 2 buffers, first must hold the clear vector and second the image being cleared
    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (desc0.is_formatted || !desc1.is_formatted || desc0.is_written || !desc1.is_written) {
        return false;
    }
    if (info.buffers.size() == 3 && info.buffers[2].buffer_type != Shader::BufferType::Flatbuf) {
        return false;
    }

    // First buffer must have size of vec4 and second the size of a single layer
    const AmdGpu::Buffer buf0 = desc0.GetSharp(info);
    const AmdGpu::Buffer buf1 = desc1.GetSharp(info);
    const u32 buf1_bpp = AmdGpu::NumBitsPerBlock(buf1.GetDataFmt());
    if (buf0.GetSize() != 16 || (cs_pgm.dim_x * 128ULL * (buf1_bpp / 8)) != buf1.GetSize()) {
        return false;
    }

    // Find image the buffer alias
    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        return false;
    }

    // Image clear must be valid
    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    if (image1.info.guest_size != buf1.GetSize() || image1.info.num_bits != buf1_bpp ||
        image1.info.props.is_depth) {
        return false;
    }

    // Perform image clear
    const float* values = reinterpret_cast<float*>(buf0.base_address);
    const vk::ClearValue clear = {
        .color = {.float32 = std::array<float, 4>{values[0], values[1], values[2], values[3]}},
    };
    const VideoCore::SubresourceRange range = {
        .base =
            {
                .level = 0,
                .layer = 0,
            },
        .extent = image1.info.resources,
    };
    runtime.ClearImage(&image1, range, clear);
    return true;
}

// uc1 (white CAUTION crates, GOW3_CAPTURE_LEAVES=<ps hex>[:<dword>], diagnosis): for a captured
// flat of that shader, each GPU-written leaf dword (where it comes from, its ring value, guest value,
// who holds it) and 64 dwords of guest memory at the pointer in flat[<dword>..+1] (default 16).
void Rasterizer::CaptureFlatSources(const Shader::Info& stage, const RingBinding& ring,
                                    const u8* captured, u64 n) {
    static const char* env = std::getenv("GOW3_CAPTURE_LEAVES");
    if (!env || !env[0]) {
        return;
    }
    static const u32 want = u32(std::strtoull(env, nullptr, 16));
    static const u32 ptr_dword = [] {
        const char* colon = std::strchr(env, ':');
        return colon ? u32(std::strtoul(colon + 1, nullptr, 10)) : 16u;
    }();
    if (u32(stage.pgm_hash) != want) {
        return;
    }
    std::string out = "    leaves:";
    for (u32 i = 0; i < num_ring_stages; ++i) {
        for (u32 j = 0; j < ring_stages[i].num_leaves; ++j) {
            const auto& leaf = ring_stages[i].leaves[j];
            if (leaf.ring_offset < ring.offset || leaf.ring_offset + 4 > ring.offset + n) {
                continue;
            }
            const u64 at = leaf.ring_offset - ring.offset;
            u32 ring_value = 0;
            std::memcpy(&ring_value, constant_ring->Data(leaf.ring_offset), 4);
            u32 guest_value = 0;
            std::memcpy(&guest_value, captured + at, 4);
            const bool gpu = IsGpuWritten(leaf.src, 4);
            out += std::format("\n      [{:3}] src {:#x} ring {:08x} guest {:08x}{}{}", at / 4,
                               leaf.src, ring_value, guest_value, gpu ? " gpu-written " : "",
                               gpu ? DescribeGpuWritten(leaf.src, 4) : "");
        }
    }
    if (u64(ptr_dword) * 4 + 8 <= n) {
        u64 ptr = 0;
        std::memcpy(&ptr, captured + ptr_dword * 4, 8);
        out += std::format("\n    flat[{}] pointer {:#x}", ptr_dword, ptr);
        if (ptr && memory->IsValidGpuMapping(ptr, 0)) {
            std::array<u32, 64> words{};
            memory->CopySparseMemory(ptr, reinterpret_cast<u8*>(words.data()), sizeof(words));
            const bool gpu = IsGpuWritten(ptr, sizeof(words));
            out += std::format(", guest now{}{}:", gpu ? ", gpu-written " : ", not gpu-written",
                               gpu ? DescribeGpuWritten(ptr, sizeof(words)) : "");
            for (u32 k = 0; k < words.size(); ++k) {
                if (k % 8 == 0) {
                    out += std::format("\n      [{:3}]", k);
                }
                out += std::format(" {:08x}", words[k]);
            }
        }
    }
    FrameCapture::BufferText(std::move(out));
}

void Rasterizer::BindBuffers(const Shader::Info& stage, const PreparedStage* prepared,
                             Shader::Backend::Bindings& binding,
                             Shader::PushData& push_data, u32& write_index) {
    const u64 alignment = instance.StorageMinAlignment();
    for (u32 buffer_index = 0; buffer_index < stage.buffers.size(); ++buffer_index) {
        const auto& desc = stage.buffers[buffer_index];
        const auto src_access =
            vk::AccessFlagBits2::eShaderRead |
            (desc.is_written ? vk::AccessFlagBits2::eShaderWrite : vk::AccessFlagBits2::eNone);
        const RingBinding* ring = num_ring_stages ? FindRingBinding(stage, buffer_index) : nullptr;
        if (ring) {
            // Copied by the GPU command thread into the constant ring (read-only).
            if (FrameCapture::Active() && ring->address == 0 && constant_ring) {
                // uc3 (sand diagnosis): a flat buffer, copied into the ring by stage A. Its
                // GPU-written dwords are still 0 there; they are read from guest memory instead
                // (the value before this submission's GPU work).
                FrameCapture::CapturedBytes bytes{};
                const u64 n = std::min<u64>(ring->size, bytes.size());
                std::memcpy(bytes.data(), constant_ring->Data(ring->offset), n);
                for (u32 i = 0; i < num_ring_stages; ++i) {
                    for (u32 j = 0; j < ring_stages[i].num_leaves; ++j) {
                        const auto& leaf = ring_stages[i].leaves[j];
                        if (leaf.ring_offset >= ring->offset && leaf.ring_offset + 4 <= ring->offset + n &&
                            memory->IsValidGpuMapping(leaf.src, 0)) {
                            memory->CopySparseMemory(leaf.src, bytes.data() + (leaf.ring_offset - ring->offset), 4);
                        }
                    }
                }
                FrameCapture::Buffer(stage.pgm_hash, buffer_index, 0, bytes.data(), ring->size);
                CaptureFlatSources(stage, *ring, bytes.data(), n);
            } else if (FrameCapture::Active() && memory->IsValidGpuMapping(ring->address, 0)) {
                // uc3: through the backing view, never the guest pointer: in precise mode a
                // GPU-written page is read-protected and the fault would be handled inline on
                // this thread, inside the binding (a deadlock after the F11 capture).
                FrameCapture::CapturedBytes bytes;
                memory->CopySparseMemory(ring->address, bytes.data(),
                                         std::min<u64>(ring->size, bytes.size()));
                FrameCapture::Buffer(stage.pgm_hash, buffer_index, ring->address, bytes.data(),
                                     ring->size);
            }
            if (ring_prep_verify) {
                if (ring->address != 0) {
                    VerifyRingBinding(*ring, nullptr);
                } else if (stage.FlatUserData().size() * sizeof(u32) >= ring->size) {
                    VerifyRingBinding(*ring, stage.FlatUserData().data());
                }
            }
            if (!desc.IsSpecial()) {
                if (ring->size == 864 && gbuffer_draw &&
                    memory->IsValidGpuMapping(ring->address, 0)) {
                    camera_motion->OnConstants(reinterpret_cast<const float*>(ring->address));
                }
                push_data.AddOffset(binding.buffer, 0);
            }
            buffer_infos.emplace_back(constant_ring->Handle(), ring->offset, ring->size);
            if (desc.buffer_type == Shader::BufferType::Flatbuf && RingLeaves(stage)) {
                // GPU copies wrote into this range: transfer write -> shader read.
                const auto* ring_buffer = constant_ring->Object();
                needs_barrier |=
                    runtime.IsBufferAccessed(ring_buffer, ring->offset, ring->size);
                bound_buffers.emplace_back(ring_buffer, ring->offset, ring->size, src_access);
            }
        } else if (desc.IsSpecial()) {
            if (desc.buffer_type == Shader::BufferType::GdsBuffer) {
                const auto* gds_buf = buffer_cache.GetGdsBuffer();
                buffer_infos.emplace_back(gds_buf->Handle(), 0, gds_buf->SizeBytes());
                needs_barrier |=
                    runtime.IsBufferAccessed(gds_buf, 0, gds_buf->SizeBytes(), desc.is_written);
                bound_buffers.emplace_back(gds_buf, 0, gds_buf->SizeBytes(), src_access);
            } else if (desc.buffer_type == Shader::BufferType::Flatbuf) {
                if (num_ring_stages == 0 && !stage.gpu_leaves.empty()) {
                    // Recorded without the draw pipeline: no GPU copy, read them back.
                    const_cast<Shader::Info&>(stage).ReadBackGpuLeaves();
                }
                auto& vk_buffer = buffer_cache.GetStreamBuffer();
                const u32 ubo_size = stage.FlatUserData().size() * sizeof(u32);
                if (FrameCapture::Active()) {
                    FrameCapture::Buffer(stage.pgm_hash, binding.buffer, 0,
                                         stage.FlatUserData().data(), ubo_size);
                }
                const u64 offset =
                    vk_buffer.Copy(stage.FlatUserData().data(), ubo_size, alignment);
                buffer_infos.emplace_back(vk_buffer.Handle(), offset, ubo_size);
            } else if (desc.buffer_type == Shader::BufferType::ClipPlanes) {
                // Permutations compiled without enabled planes never read the buffer, so the
                // declared binding is satisfied with a null descriptor instead of a copy.
                if (Regs().clipper_control.user_clip_plane_enable == 0) {
                    buffer_infos.emplace_back(VK_NULL_HANDLE, 0, VK_WHOLE_SIZE);
                } else {
                    auto& vk_buffer = buffer_cache.GetStreamBuffer();
                    std::array<float, AmdGpu::NUM_CLIP_PLANES * 4> planes{};
                    for (u32 i = 0; i < AmdGpu::NUM_CLIP_PLANES; ++i) {
                        const auto& plane = Regs().clip_user_data[i];
                        planes[i * 4 + 0] = std::bit_cast<float>(plane.data_x);
                        planes[i * 4 + 1] = std::bit_cast<float>(plane.data_y);
                        planes[i * 4 + 2] = std::bit_cast<float>(plane.data_z);
                        planes[i * 4 + 3] = std::bit_cast<float>(plane.data_w);
                    }
                    const u32 ubo_size = static_cast<u32>(sizeof(planes));
                    const u64 offset = vk_buffer.Copy(planes.data(), ubo_size, alignment);
                    buffer_infos.emplace_back(vk_buffer.Handle(), offset, ubo_size);
                }
            } else if (desc.buffer_type == Shader::BufferType::BdaPagetable) {
                const auto* bda_buffer = buffer_cache.GetBdaPageTableBuffer();
                buffer_infos.emplace_back(bda_buffer->Handle(), 0, bda_buffer->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::FaultBuffer) {
                const auto* fault_buffer = buffer_cache.GetFaultBuffer();
                buffer_infos.emplace_back(fault_buffer->Handle(), 0, fault_buffer->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::SharedMemory) {
                auto& lds_buffer = buffer_cache.GetStreamBuffer();
                const auto& cs_program = CsRegs();
                const auto lds_size = cs_program.SharedMemSize() * cs_program.NumWorkgroups();
                const auto [data, offset] = lds_buffer.Map(lds_size, alignment);
                std::memset(data, 0, lds_size);
                lds_buffer.Commit();
                buffer_infos.emplace_back(lds_buffer.Handle(), offset, lds_size);
            } else {
                UNREACHABLE_MSG("Unexpected buffer type {}", u32(desc.buffer_type));
            }
        } else {
            const auto vsharp =
                prepared ? prepared->buffer_sharps[buffer_index] : desc.GetSharp(stage);
            if (vsharp.GetSize() == 864 && gbuffer_draw &&
                memory->IsValidGpuMapping(vsharp.base_address, 0)) {
                camera_motion->OnConstants(reinterpret_cast<const float*>(vsharp.base_address));
            }
            // Object motion research: the vertex shader buffers of G-buffer draws.
            if (FrameCapture::Active() && gbuffer_draw && stage.sw_stage == Shader::SwStage::Vertex &&
                vsharp.base_address != 0 && memory->IsValidGpuMapping(vsharp.base_address, 0)) {
                const auto* f = reinterpret_cast<const float*>(vsharp.base_address);
                char note[192];
                std::snprintf(note, sizeof(note),
                              "\n  gb vs %08x slot %u at %#llx size %u stride %u: %g %g %g %g",
                              u32(stage.pgm_hash), binding.buffer,
                              (unsigned long long)vsharp.base_address, u32(vsharp.GetSize()),
                              u32(vsharp.GetStride()), f[0], f[1], f[2], f[3]);
                FrameCapture::Note(note);
            }
            if (FrameCapture::Active() && vsharp.base_address != 0 && vsharp.GetSize() != 0 &&
                memory->IsValidGpuMapping(vsharp.base_address, 0)) {
                // uc3: backing view, not the guest pointer (see the ring case above).
                const u64 size = memory->ClampRangeSize(vsharp.base_address, vsharp.GetSize());
                FrameCapture::CapturedBytes bytes;
                memory->CopySparseMemory(vsharp.base_address, bytes.data(),
                                         std::min<u64>(size, bytes.size()));
                FrameCapture::Buffer(stage.pgm_hash, binding.buffer, vsharp.base_address,
                                     bytes.data(), size);
            }
            if (vsharp.base_address == 0 || vsharp.GetSize() == 0) {
                buffer_infos.emplace_back(VK_NULL_HANDLE, 0, VK_WHOLE_SIZE);
            } else {
                const u64 size = memory->ClampRangeSize(vsharp.base_address, vsharp.GetSize());
                if (size != vsharp.GetSize()) {
                    LOG_ERROR(Render, "Clamped size from {} to {} for stage {:#x}",
                              vsharp.GetSize(), size, stage.pgm_hash);
                }
                StageBTimer obtain_timer{SbObtain};
                const auto [buffer, offset] = buffer_cache.ObtainBuffer(
                    vsharp.base_address, size, desc.is_written, desc.is_formatted);
                obtain_timer.Stop();
                const u64 offset_aligned = Common::AlignDown(offset, alignment);
                const u64 adjust = offset - offset_aligned;
                if (adjust % 4 != 0) {
                    LOG_WARNING(Render_Vulkan, "Buffer binding in shader {:#x} isn't dword aligned",
                                stage.pgm_hash);
                }
                push_data.AddOffset(binding.buffer, adjust);
                buffer_infos.emplace_back(buffer->Handle(), offset_aligned, size + adjust);
                // uc3: a formatted buffer with a stride touches one element per record, so its
                // last byte is (records - 1) * stride + element size, not records * stride. The
                // game binds one array of structs several times at field offsets (0, 0xC, 0x14:
                // shader 8f067540 in the streets, ~1700 dispatches a frame); tracked at the full
                // size, the last field's range ran into the next dispatch's array and every
                // dispatch got a barrier. GOW3_TIGHT_FORMATTED=0 tracks the full size again.
                static const bool tight_formatted = [] {
                    const char* env = std::getenv("GOW3_TIGHT_FORMATTED");
                    return !env || env[0] != '0';
                }();
                u64 track_size = size;
                if (tight_formatted && desc.is_formatted && vsharp.GetStride() != 0) {
                    const u32 element = AmdGpu::NumBitsPerBlock(vsharp.GetDataFmt()) / 8;
                    if (element != 0 && element < vsharp.GetStride() && size == vsharp.GetSize()) {
                        track_size = size - (vsharp.GetStride() - element);
                    }
                }
                bound_buffers.emplace_back(buffer, offset, track_size, src_access);
                if (desc.is_written) {
                    // Raw storage-buffer writes can also make an aliased cached image stale.
                    static const VAddr watch_addr = [] {
                        const char* env = std::getenv("GOW3_WATCH_ADDR");
                        return env ? VAddr(std::strtoull(env, nullptr, 16)) : VAddr(0);
                    }();
                    if (watch_addr && vsharp.base_address < watch_addr + 0x20000 &&
                        vsharp.base_address + size > watch_addr) {
                        static std::atomic<int> shown{0};
                        if (shown.fetch_add(1) < 200) {
                            std::printf("ucndc: watch buffer store shader %#x base %#llx size %#x\n",
                                        u32(stage.pgm_hash), (unsigned long long)vsharp.base_address,
                                        u32(size));
                            std::fflush(stdout);
                        }
                    }
                    StageBTimer timer{SbInvalidate};
                    texture_cache.InvalidateMemoryFromGPU(vsharp.base_address, size);
                }
                StageBTimer accessed_timer{SbAccessed};
                const bool accessed =
                    runtime.IsBufferAccessed(buffer, offset, track_size, desc.is_written);
                accessed_timer.Stop();
                needs_barrier |= accessed;
                // uc3: GOW3_BARRIER_TRACE=<shader hash, hex>: the first 300 buffer bindings of
                // that shader, and whether each one needed a barrier.
                static const u64 trace_hash = [] {
                    const char* env = std::getenv("GOW3_BARRIER_TRACE");
                    return env ? std::strtoull(env, nullptr, 16) : 0ull;
                }();
                static std::atomic<int> traced{0};
                if (trace_hash && stage.pgm_hash == trace_hash &&
                    traced.fetch_add(1, std::memory_order_relaxed) < 300) {
                    std::printf("barrier trace %016llx: binding %u addr=0x%llx size=%llu written=%d "
                                "formatted=%d accessed=%d\n",
                                (unsigned long long)stage.pgm_hash, binding.buffer,
                                (unsigned long long)vsharp.base_address, (unsigned long long)size,
                                int(desc.is_written), int(desc.is_formatted), int(accessed));
                }
                // uc3: GOW3_WRITE_TRACE=<guest address, hex>: the first 40 shader bindings
                // that write it (who produces the data a GPU-side read fault waits for);
                // =1: the 4 KiB page of the last GPU-side read fault (GOW3_FAULT_TRACE=1).
                static const u64 write_trace_env = [] {
                    const char* env = std::getenv("GOW3_WRITE_TRACE");
                    return env ? std::strtoull(env, nullptr, 16) : 0ull;
                }();
                const u64 write_trace =
                    write_trace_env == 1
                        ? Gow3Stats::uc3_last_gpu_fault.load(std::memory_order_relaxed) & ~0xfffull
                        : write_trace_env;
                static std::atomic<int> write_traced{0};
                if (write_trace && desc.is_written && vsharp.base_address < write_trace + 0x1000 &&
                    write_trace < vsharp.base_address + size &&
                    write_traced.fetch_add(1, std::memory_order_relaxed) < 40) {
                    std::printf("write trace: shader %016llx binding %u addr=0x%llx size=%llu "
                                "stride=%u tick=%llu\n",
                                (unsigned long long)stage.pgm_hash, binding.buffer,
                                (unsigned long long)vsharp.base_address, (unsigned long long)size,
                                vsharp.GetStride(), (unsigned long long)scheduler.CurrentTick());
                }
            }
        }

        auto& set_write = set_writes[write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified++;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = 1;
        set_write.descriptorType = vk::DescriptorType::eStorageBuffer;
        set_write.pBufferInfo = &buffer_infos.back();
        ++binding.buffer;
    }
}

Rasterizer::ImageDescCacheEntry& Rasterizer::CachedImageDescEntry(const AmdGpu::Image& sharp,
                                                                   const Shader::ImageResource& res,
                                                                   u64 hash) {
    std::array<u64, 4> key;
    std::memcpy(key.data(), &sharp, sizeof(key));
    const u32 flags = u32(res.is_written) | u32(res.is_depth) << 1 | u32(res.is_array) << 2;
    // Two-way set associative: a frame binds a few thousand distinct T#s.
    auto* set = &image_desc_cache[(hash % (image_desc_cache.size() / 2)) * 2];
    const auto matches = [&](const ImageDescCacheEntry& e) {
        return e.flags == flags && e.sharp == key;
    };
    const bool disabled = Gow3Toggle::Disabled(Gow3Toggle::ImageDescCache);
    ImageDescCacheEntry* slot = nullptr;
    if (!disabled && matches(set[0])) {
        slot = &set[0];
    } else if (!disabled && matches(set[1])) {
        slot = &set[1];
    } else {
        // Replace the least recently used way that no binding of this call points at.
        slot = set[0].last_use <= set[1].last_use ? &set[0] : &set[1];
        if (slot->pinned == bind_epoch) {
            slot = slot == &set[0] ? &set[1] : &set[0];
        }
        if (slot->pinned == bind_epoch) {
            slot = &image_desc_overflow.emplace_back();
        }
    }
    slot->last_use = ++desc_use_counter;
    auto& entry = *slot;
    if (entry.flags != flags || entry.sharp != key ||
        Gow3Toggle::Disabled(Gow3Toggle::ImageDescCache)) {
        entry.desc = VideoCore::TextureCache::ImageDesc{sharp, res};
        entry.sharp = key;
        entry.flags = flags;
        entry.found_generation = ~0ULL;
        entry.view_memo = {};
    }
    return entry;
}

void Rasterizer::BindTextures(const Shader::Info& stage, const PreparedStage* prepared,
                              Shader::Backend::Bindings& binding, u32& write_index,
                              bool& barrier, bool on_helper) {
    const u32 first_image_idx = image_infos.size();
    TextureSet* set_slot = nullptr;
    if (!on_helper && BindTexturesFromSet(stage, prepared, first_image_idx, barrier, set_slot)) {
        // Descriptor writes: one per image (no mip arrays in a memoized set).
        for (u32 i = 0; i < stage.images.size(); ++i) {
            auto& set_write = set_writes[write_index++];
            set_write.dstSet = VK_NULL_HANDLE;
            set_write.dstBinding = binding.unified++;
            set_write.dstArrayElement = 0;
            set_write.descriptorCount = 1;
            set_write.descriptorType = vk::DescriptorType::eSampledImage;
            set_write.pImageInfo = &image_infos[first_image_idx + i];
        }
        BindSamplers(stage, prepared, binding, write_index);
        return;
    }
    image_bindings.clear();
    image_binding_entries.clear();
    image_desc_overflow.clear();
    image_desc_storage.clear();
    ++bind_epoch;
    // To emulate storing to explicit mip levels, build a descriptor array with each mip level.
    boost::container::small_vector<u32, 8> image_descriptor_array_sizes;
    // TextureSetMemo: what this call resolves, to remember when the set qualifies.
    std::array<TextureSetEntry, TextureSet::MaxImages> resolved{};
    bool set_ok = set_slot != nullptr;
    // SampleSceneProxies: bindings that may read a reduced scene proxy instead of the native
    // image (normalized sampling only; see Shader::ImageResource::needs_native).
    boost::container::small_vector<bool, 16> binding_proxy_ok;
    const bool sample_proxies = !Gow3Toggle::Disabled(Gow3Toggle::SampleSceneProxies) &&
                                !on_helper && scene_targets->Reduced();

    for (u32 image_index = 0; image_index < stage.images.size(); ++image_index) {
        const auto& image_desc = stage.images[image_index];
        bool movie_sharp = false;
        const auto tsharp = [&] {
            auto sharp = prepared ? prepared->image_sharps[image_index] : image_desc.GetSharp(stage);
            movie_sharp = Uc3MovieSharp(stage, image_index, sharp);
            return sharp;
        }();
        // gow3: a hash lookup per texture per draw for a diagnostic only.
        static const bool warn_meta = std::getenv("GOW3_WARN_META_TEXTURE") != nullptr;
        if (warn_meta && texture_cache.IsMeta(tsharp.Address())) {
            LOG_WARNING(Render_Vulkan, "Unexpected metadata read by a shader (texture)");
        }

        const auto data_fmt = tsharp.GetDataFmt();
        const auto num_fmt = tsharp.GetNumberFmt();
        if (tsharp.Address() == 0 || data_fmt == AmdGpu::DataFormat::FormatInvalid) {
            image_bindings.emplace_back(VideoCore::ImageId{}, &image_desc_storage.emplace_back());
            image_binding_entries.push_back(nullptr);
            binding_proxy_ok.push_back(false);
            image_descriptor_array_sizes.push_back(1);
            continue;
        }

        if (!memory->IsValidGpuMapping(tsharp.Address(), 0) ||
            !IsKnownFormat(data_fmt, num_fmt)) {
            LOG_WARNING(Render_Vulkan,
                        "Rejecting invalid T# address={:#x}, pitch={}, width={}, "
                        "data_format={}, num_format={}",
                        tsharp.Address(), tsharp.pitch, tsharp.width, static_cast<u32>(data_fmt),
                        static_cast<u32>(num_fmt));
            image_bindings.emplace_back(VideoCore::ImageId{}, &image_desc_storage.emplace_back());
            image_binding_entries.push_back(nullptr);
            binding_proxy_ok.push_back(false);
            image_descriptor_array_sizes.push_back(1);
            continue;
        }

        const Shader::MipStorageFallbackMode mip_fallback_mode = image_desc.mip_fallback_mode;
        // Same as image_desc.NumBindings(stage), without fetching the T# again.
        const u32 num_bindings =
            mip_fallback_mode == Shader::MipStorageFallbackMode::DynamicIndex
                ? static_cast<u32>(tsharp.last_level - tsharp.base_level + 1)
                : 1u;
        const bool proxy_candidate = sample_proxies && num_bindings == 1 &&
                                     !image_desc.needs_native && !image_desc.is_written &&
                                     mip_fallback_mode == Shader::MipStorageFallbackMode::None;

        auto& desc_entry =
            prepared && !movie_sharp
                ? CachedImageDescEntry(tsharp, image_desc, prepared->image_hashes[image_index])
                : CachedImageDescEntry(tsharp, image_desc);
        // GOW3_SCENE_DEBUG: proxied scene targets this binding reads at the native size, and why.
        const auto debug_native = [&](const VideoCore::Image& image) {
            if (!scene_debug_frame || proxy_candidate || !image.scene_proxy) return;
            std::printf("Scene native read: %s %016llx %s %ux%u needs_native %d written %d "
                        "bindings %u mip_fallback %d\n",
                        stage.sw_stage == Shader::SwStage::Compute ? "cs" : "gfx",
                        (unsigned long long)stage.pgm_hash,
                        vk::to_string(image.info.pixel_format).c_str(), image.info.size.width,
                        image.info.size.height, int(image_desc.needs_native),
                        int(image_desc.is_written), num_bindings, int(mip_fallback_mode));
        };
        for (auto i = 0; i < num_bindings; i++) {
            // gow3: a plain binding (no mip override) of the same T# resolves to the same image
            // while no image was registered or unregistered.
            if (mip_fallback_mode == Shader::MipStorageFallbackMode::None &&
                desc_entry.found_generation == texture_cache.RegistryGeneration() &&
                !Gow3Toggle::Disabled(Gow3Toggle::TextureBindingMemo)) {
                desc_entry.pinned = bind_epoch;
                auto& [image_id, _] =
                    image_bindings.emplace_back(desc_entry.found_id, &desc_entry.found_desc);
                image_binding_entries.push_back(&desc_entry);
                binding_proxy_ok.push_back(proxy_candidate);
                texture_cache.MarkFound(image_id);
                auto* image = &texture_cache.GetImage(image_id);
                if (auto depth_image_id = texture_cache.GetAssociatedDepth(
                        *image, desc_entry.found_desc.view_info.format)) {
                    image_id = depth_image_id;
                    image = &texture_cache.GetImage(image_id);
                }
                debug_native(*image);
                if (image->binding.is_bound) {
                    image->binding.force_general |= image_desc.is_written;
                }
                image->binding.is_bound = 1u;
                continue;
            }
            auto& desc = image_desc_storage.emplace_back(desc_entry.desc);
            auto& [image_id, _] = image_bindings.emplace_back(VideoCore::ImageId{}, &desc);
            image_binding_entries.push_back(nullptr);
            binding_proxy_ok.push_back(proxy_candidate);

            if (mip_fallback_mode == Shader::MipStorageFallbackMode::ConstantIndex) {
                ASSERT(num_bindings == 1);
                desc.view_info.range.base.level += image_desc.constant_mip_index;
                desc.view_info.range.extent.levels = 1;
            } else if (mip_fallback_mode == Shader::MipStorageFallbackMode::DynamicIndex) {
                desc.view_info.range.base.level += i;
                desc.view_info.range.extent.levels = 1;
            }

            const u64 generation = texture_cache.RegistryGeneration();
            image_id = texture_cache.FindImage(desc);
            if (mip_fallback_mode == Shader::MipStorageFallbackMode::None &&
                generation == texture_cache.RegistryGeneration() &&
                desc_entry.pinned != bind_epoch) {
                desc_entry.found_generation = generation;
                desc_entry.found_id = image_id;
                desc_entry.found_desc = desc;
                desc_entry.view_memo = {};
            }
            auto* image = &texture_cache.GetImage(image_id);
            if (auto depth_image_id =
                    texture_cache.GetAssociatedDepth(*image, desc.view_info.format)) {
                // If this image has an associated depth image, it's a stencil attachment.
                // Redirect the access to the actual depth-stencil buffer.
                image_id = depth_image_id;
                image = &texture_cache.GetImage(image_id);
            }
            debug_native(*image);
            if (image->binding.is_bound) {
                // The image is already bound. In case if it is about to be used as storage we
                // need to force general layout on it.
                image->binding.force_general |= image_desc.is_written;
            }
            image->binding.is_bound = 1u;
        }

        image_descriptor_array_sizes.push_back(num_bindings);
    }

    // Second pass to re-bind images that were updated after binding
    for (u32 binding_index = 0; binding_index < image_bindings.size(); ++binding_index) {
        auto& [image_id, desc_ptr] = image_bindings[binding_index];
        auto* memo_entry = image_binding_entries[binding_index];
        const auto& desc = *desc_ptr;
        bool is_storage = desc.type == VideoCore::TextureCache::BindingType::Storage;
        if (!image_id) {
            image_infos.emplace_back(VK_NULL_HANDLE, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
        } else {
            if (auto& old_image = texture_cache.GetImage(image_id);
                old_image.binding.needs_rebind) {
                old_image.binding = {};
                auto& rebind_desc = image_desc_storage.emplace_back(desc);
                desc_ptr = &rebind_desc;
                image_id = texture_cache.FindImage(rebind_desc);
                memo_entry = nullptr;
                set_ok = false;
            }

            bound_images.emplace_back(image_id);

            auto& image = texture_cache.GetImage(image_id);
            auto& image_view = texture_cache.FindTexture(
                image_id, desc, memo_entry ? &memo_entry->view_memo : nullptr, !on_helper);
            const auto binding = image.binding;

            // gow3: a proxied scene image sampled with normalized coordinates reads the proxy:
            // no resample to the native size (the native image is not touched).
            if (binding_proxy_ok[binding_index] && !is_storage && !binding.force_general &&
                !binding.is_target &&
                !(upscaler->Enabled() && upscaler->RedirectsSampled(image_id))) {
                if (const auto proxy = scene_targets->SampleProxy(image, desc.view_info)) {
                    image.usage.texture = 1u;
                    image_infos.emplace_back(VK_NULL_HANDLE, proxy->view, proxy->layout);
                    if (set_ok && binding_index < resolved.size()) {
                        resolved[binding_index] = {image_id, proxy->view, image.backing,
                                                   desc.view_info.range, true};
                    }
                    ++proxy_samples;
                    continue;
                }
            }
            // The image is either bound as storage in a separate descriptor or bound as render
            // target in feedback loop. Depth images are excluded because they can't be bound as
            // storage and feedback loop doesn't make sense for them
            if ((binding.force_general || binding.is_target) && !image.info.props.is_depth) {
                if (binding.is_target &&
                    FeedbackTraceFirst((image.image_uid << 16) | (u64(desc.view_info.range.base.level) << 8) |
                                       (u64(desc.view_info.range.extent.levels) << 1))) {
                    std::printf("MIP_FIX trace: sampled while a render target: %#llx %ux%u fmt %s levels %u, "
                                "sampled mips %u+%u, layout %s, %s, feedback usage %d\n",
                                (unsigned long long)image.info.guest_address, image.info.size.width,
                                image.info.size.height, vk::to_string(image.info.pixel_format).c_str(),
                                u32(image.info.resources.levels), u32(desc.view_info.range.base.level),
                                u32(desc.view_info.range.extent.levels),
                                vk::to_string(image.backing->state.layout).c_str(),
                                instance.IsAttachmentFeedbackLoopLayoutSupported()
                                    ? "feedback loop layout" : "general",
                                int(bool(image.usage_flags & vk::ImageUsageFlagBits::eAttachmentFeedbackLoopEXT)));
                }
                if (instance.IsAttachmentFeedbackLoopLayoutSupported() && image.binding.is_target) {
                    barrier |= runtime.Transit(
                        &image, vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT,
                        vk::PipelineStageFlagBits2::eAllGraphics, vk::AccessFlagBits2::eShaderRead);
                } else {
                    barrier |= runtime.Transit(
                        &image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eAllCommands,
                        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                        desc.view_info.range);
                }
            } else {
                if (is_storage) {
                    barrier |= runtime.Transit(
                        &image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eAllCommands,
                        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                        desc.view_info.range);
                } else {
                    const auto new_layout = image.info.props.is_depth
                                                ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                                : vk::ImageLayout::eShaderReadOnlyOptimal;
                    barrier |= runtime.Transit(
                        &image, new_layout, vk::PipelineStageFlagBits2::eAllCommands,
                        vk::AccessFlagBits2::eShaderRead, desc.view_info.range);
                }
            }
            image.usage.storage |= is_storage;
            image.usage.texture |= !is_storage;
            if (FrameCapture::Active()) {
                const auto& used = image_view.info.range;
                FrameCapture::Sampled(
                    image.info, is_storage,
                    std::format("image mips {} layers {}, T# mips {} layers {}, view mip {}+{} "
                                "layer {}+{} min_lod {}",
                                image.info.resources.levels, image.info.resources.layers,
                                desc.info.resources.levels, desc.info.resources.layers,
                                used.base.level, used.extent.levels, used.base.layer,
                                used.extent.layers, image_view.info.min_lod));
            }

            vk::ImageView view = *image_view.image_view;
            vk::ImageLayout layout = image.backing->state.layout;
            if (upscaler->Enabled()) {
                // gow3: the display pass reads the upscaled frame (scaled presets).
                upscaler->RedirectSampled(image_id, image_view.info, view, layout);
            }
            image_infos.emplace_back(VK_NULL_HANDLE, view, layout);
            if (set_ok && binding_index < resolved.size()) {
                resolved[binding_index] = {image_id, *image_view.image_view, image.backing,
                                           desc.view_info.range};
                set_ok = !is_storage && !binding.force_general && !binding.is_target;
            }
        }
    }
    if (set_ok && image_bindings.size() == stage.images.size()) {
        // Remember the set: the same T#s resolve the same way while no image is (un)registered.
        set_slot->generation = texture_cache.RegistryGeneration();
        set_slot->scene_generation = scene_targets->Generation();
        set_slot->count = static_cast<u32>(stage.images.size());
        std::copy_n(resolved.begin(), set_slot->count, set_slot->entries.begin());
    } else if (set_slot) {
        set_slot->key = 0;
    }

    u32 image_info_idx = first_image_idx;
    u32 image_binding_idx = 0;
    for (u32 array_size : image_descriptor_array_sizes) {
        const auto& desc = *image_bindings[image_binding_idx].second;
        const bool is_storage = desc.type == VideoCore::TextureCache::BindingType::Storage;
        auto& set_write = set_writes[write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = array_size;
        set_write.descriptorType =
            is_storage ? vk::DescriptorType::eStorageImage : vk::DescriptorType::eSampledImage;
        set_write.pImageInfo = &image_infos[image_info_idx];

        image_info_idx += array_size;
        image_binding_idx += array_size;
        binding.unified += array_size;
    }

    BindSamplers(stage, prepared, binding, write_index);
}

void Rasterizer::BindSamplers(const Shader::Info& stage, const PreparedStage* prepared,
                              Shader::Backend::Bindings& binding, u32& write_index) {
    for (u32 sampler_index = 0; sampler_index < stage.samplers.size(); ++sampler_index) {
        const auto& sampler = stage.samplers[sampler_index];
        auto ssharp =
            prepared ? prepared->sampler_sharps[sampler_index] : sampler.GetSharp(stage);
        // ucndc (GOW3_NO_SSHARP_REJECT=1, diagnosis): keep every S# as the game wrote it, as
        // SuperMedo's port does. The default S# compares with Never, so a rejected shadow
        // sampler would make everything read as shadowed.
        static const bool keep_ssharp = [] {
            const char* env = std::getenv("GOW3_NO_SSHARP_REJECT");
            return env && env[0] == '1';
        }();
        if (!keep_ssharp &&
            (!ssharp.Valid() || (ssharp.border_color_type.Value() == AmdGpu::BorderColor::Custom &&
                                 Regs().ta_bc_base.Address() == 0))) {
            static std::atomic<u32> reported{0};
            if (reported.fetch_add(1, std::memory_order_relaxed) < 20) {
                std::fprintf(stderr, "ucndc: rejected S# (max_aniso=%u filter_mode=%u mip_filter=%u border=%u)\n",
                             static_cast<u32>(ssharp.max_aniso.Value()),
                             static_cast<u32>(ssharp.filter_mode.Value()),
                             static_cast<u32>(ssharp.mip_filter.Value()),
                             static_cast<u32>(ssharp.border_color_type.Value()));
            }
            LOG_WARNING(Render_Vulkan,
                        "Rejecting invalid S# max_aniso={}, filter_mode={}, mip_filter={}, "
                        "border_color_type={}, border_color_base={:#x}",
                        static_cast<u32>(ssharp.max_aniso.Value()),
                        static_cast<u32>(ssharp.filter_mode.Value()),
                        static_cast<u32>(ssharp.mip_filter.Value()),
                        static_cast<u32>(ssharp.border_color_type.Value()),
                        Regs().ta_bc_base.Address());
            ssharp = AmdGpu::Sampler{};
        }
        const float extra_bias = sampler.is_depth ? 0.0f : sampler_lod_bias;
        const auto vk_sampler =
            texture_cache.GetSampler(ssharp, Regs().ta_bc_base, sampler.is_depth, extra_bias);
        if (FrameCapture::Active()) {
            FrameCapture::SamplerState(std::format(
                "sampler {} raw {:016x} {:016x}: mag {} min {} mip {} aniso {} (threshold {}) "
                "lod bias {:.3f} (secondary {}) + extra {:.2f}, lod {:.3f}..{:.3f}, perf_mip {}, "
                "filter_mode {}, depth {}",
                sampler_index, u64(ssharp.raw0), u64(ssharp.raw1), u32(ssharp.xy_mag_filter.Value()),
                u32(ssharp.xy_min_filter.Value()), u32(ssharp.mip_filter.Value()),
                u32(ssharp.max_aniso.Value()), u32(ssharp.aniso_threshold.Value()),
                ssharp.LodBias(), u32(ssharp.lod_bias_sec.Value()),
                extra_bias, ssharp.MinLod(), ssharp.MaxLod(),
                u32(ssharp.perf_mip.Value()), u32(ssharp.filter_mode.Value()),
                int(sampler.is_depth)));
        }
        image_infos.emplace_back(vk_sampler, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
        auto& set_write = set_writes[write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified++;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = 1;
        set_write.descriptorType = vk::DescriptorType::eSampler;
        set_write.pImageInfo = &image_infos.back();
    }
}

Rasterizer::BeginSignature Rasterizer::MakeBeginSignature(const GraphicsPipeline* pipeline) const {
    const auto& key = pipeline->GetGraphicsKey();
    const auto& regs = Regs();
    BeginSignature sig;
    const u32 num_color = std::bit_width(key.mrt_mask);
    for (u32 cb = 0; cb < num_color && cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
        sig.ids[cb] = cb_descs[cb].first;
        if (cb_descs[cb].first) {
            sig.views[cb] = cb_descs[cb].second.view_info;
        }
        sig.color_samples[cb] = key.color_samples[cb];
    }
    sig.ids[AmdGpu::NUM_COLOR_BUFFERS] = db_desc.first;
    if (db_desc.first) {
        sig.views[AmdGpu::NUM_COLOR_BUFFERS] = db_desc.second.view_info;
    }
    sig.mrt_mask = key.mrt_mask;
    sig.num_samples = key.num_samples;
    sig.motion = key.motion_vectors;
    sig.scene_started = scene_started;
    sig.raster_scaling = upscaler->RasterScaling();
    sig.upscaler_state = upscaler->RedirectState();
    sig.generation = texture_cache.RegistryGeneration();
    std::memcpy(&sig.depth_control, &regs.depth_control, sizeof(u32));
    sig.depth_valid = regs.depth_buffer.DepthValid();
    sig.stencil_valid = regs.depth_buffer.StencilValid();
    return sig;
}

bool Rasterizer::BindTexturesFromSet(const Shader::Info& stage, const PreparedStage* prepared,
                                     u32 first_image_idx, bool& barrier, TextureSet*& slot) {
    const u32 count = static_cast<u32>(stage.images.size());
    if (!prepared || count == 0 || count > TextureSet::MaxImages ||
        Gow3Toggle::Disabled(Gow3Toggle::TextureSetMemo) || FrameCapture::Active()) {
        return false;
    }
    for (const auto& image : stage.images) {
        if (image.is_written || image.mip_fallback_mode != Shader::MipStorageFallbackMode::None) {
            return false;
        }
    }
    u64 key = XXH3_64bits_withSeed(prepared->image_hashes, count * sizeof(u64),
                                   reinterpret_cast<u64>(&stage));
    key |= 1; // 0 marks an empty slot
    auto& set = texture_sets[key % texture_sets.size()];
    slot = &set;
    const u64 generation = texture_cache.RegistryGeneration();
    const bool match = set.key == key && set.stage == &stage && set.count == count &&
                       set.generation == generation &&
                       set.scene_generation == scene_targets->Generation() &&
                       std::equal(set.hashes.begin(), set.hashes.begin() + count,
                                  prepared->image_hashes);
    if (!match) {
        ++texture_set_why[set.key != key ? 0 : set.generation != generation ? 1 : 3];
        ++texture_set_misses;
        set.key = key;
        set.stage = &stage;
        set.count = 0;
        set.generation = ~0ull;
        std::copy_n(prepared->image_hashes, count, set.hashes.begin());
        return false;
    }
    // Every image still has the backing its view belongs to and needs no refresh.
    for (u32 i = 0; i < count; ++i) {
        const auto& entry = set.entries[i];
        if (!entry.id) {
            continue;
        }
        const auto& image = texture_cache.GetImage(entry.id);
        // Proxy entries need a current proxy; native entries of a proxied image go through
        // BindTextures, which may sample the proxy instead.
        const bool proxies_on = !Gow3Toggle::Disabled(Gow3Toggle::SampleSceneProxies);
        if (image.backing != entry.backing || image.binding.needs_rebind ||
            image.binding.is_target || !texture_cache.IsUpToDate(entry.id) ||
            (entry.proxy ? !proxies_on ||
                               !scene_targets->ProxyCurrent(image, entry.range.base.level,
                                                            entry.range.extent.levels)
                         : image.scene_proxy && proxies_on) ||
            (upscaler->Enabled() && upscaler->RedirectsSampled(entry.id))) {
            ++texture_set_why[2];
            ++texture_set_misses;
            set.generation = ~0ull;
            return false;
        }
    }
    ++texture_set_hits;
    slot = nullptr;
    for (u32 i = 0; i < count; ++i) {
        const auto& entry = set.entries[i];
        if (!entry.id) {
            image_infos.emplace_back(VK_NULL_HANDLE, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
            continue;
        }
        texture_cache.MarkFound(entry.id);
        auto& image = texture_cache.GetImage(entry.id);
        image.binding.is_bound = 1u;
        bound_images.emplace_back(entry.id);
        image.usage.texture = 1u;
        if (entry.proxy) {
            image_infos.emplace_back(VK_NULL_HANDLE, entry.view,
                                     scene_targets->PrepareSample(image, entry.range.base.level));
            ++proxy_samples;
            continue;
        }
        const auto new_layout = image.info.props.is_depth
                                    ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                    : vk::ImageLayout::eShaderReadOnlyOptimal;
        barrier |= runtime.Transit(&image, new_layout, vk::PipelineStageFlagBits2::eAllCommands,
                                   vk::AccessFlagBits2::eShaderRead, entry.range);
        image_infos.emplace_back(VK_NULL_HANDLE, entry.view, image.backing->state.layout);
    }
    (void)first_image_idx;
    return true;
}

RenderState Rasterizer::BeginRendering(const GraphicsPipeline* pipeline) {
    // gow3: RenderStateMemo (see BeginMemo).
    const bool memo_on = !Gow3Toggle::Disabled(Gow3Toggle::RenderStateMemo) && !FrameCapture::Active();
    if (Gow3Stats::enabled && ((begin_memo_hits + begin_memo_misses) & 0x3FFFF) == 0x3FFFF) {
        std::printf("Render state memo: %llu hits, %llu misses; texture sets: %llu hits, %llu "
                    "misses\n",
                    static_cast<unsigned long long>(begin_memo_hits),
                    static_cast<unsigned long long>(begin_memo_misses),
                    static_cast<unsigned long long>(texture_set_hits),
                    static_cast<unsigned long long>(texture_set_misses));
        std::printf("  texture set misses: other key %llu, generation %llu, image check %llu, "
                    "other %llu\n",
                    (unsigned long long)texture_set_why[0], (unsigned long long)texture_set_why[1],
                    (unsigned long long)texture_set_why[2], (unsigned long long)texture_set_why[3]);
        std::printf("  scene proxies sampled directly: %llu bindings\n",
                    static_cast<unsigned long long>(proxy_samples));
        proxy_samples = 0;
        texture_set_why = {};
        begin_memo_hits = begin_memo_misses = texture_set_hits = texture_set_misses = 0;
    }
    BeginSignature signature;
    if (memo_on) {
        signature = MakeBeginSignature(pipeline);
        const auto& regs = Regs();
        bool usable = begin_memo.valid && scheduler.IsRenderingWith(begin_memo.state) &&
                      !regs.depth_render_control.depth_clear_enable &&
                      !regs.depth_render_control.stencil_clear_enable &&
                      signature == begin_memo.signature;
        for (const auto id : signature.ids) {
            if (usable && id) {
                const auto& image = texture_cache.GetImage(id);
                usable = !image.binding.is_bound && !image.binding.needs_rebind;
            }
        }
        if (usable) {
            ++begin_memo_hits;
            attachment_feedback_loop = false;
            push_data.scene_size = begin_memo.scene_size;
            target_scale = begin_memo.target_scale;
            return begin_memo.state;
        }
        ++begin_memo_misses;
    }
    begin_memo.valid = false;
    RenderState state = BeginRenderingFull(pipeline);
    if (memo_on && !attachment_feedback_loop) {
        // (is_clear of the depth attachment shares its bytes with has_depth.)
        bool clears = state.depth_stencil_attachment.depth_clear ||
                      state.depth_stencil_attachment.stencil_clear;
        for (u32 i = 0; i < state.num_color_attachments; ++i) {
            clears |= state.color_attachments[i].is_clear != 0;
        }
        if (!clears) {
            // Recomputed after the full path: it may have started the scene (scene_started).
            begin_memo = {true, MakeBeginSignature(pipeline), state, push_data.scene_size,
                          target_scale};
        }
    }
    return state;
}

RenderState Rasterizer::BeginRenderingFull(const GraphicsPipeline* pipeline) {
    attachment_feedback_loop = false;
    using VulkanUpscalerTarget = TemporalUpscaler::Target;
    VulkanUpscalerTarget redirect{};
    bool color_redirected = false, depth_redirected = false;
    std::pair<u32, u32> guest_extent{1, 1};
    std::pair<vk::ImageView, vk::ImageLayout> original_color{};
    const auto& regs = Regs();
    const auto& key = pipeline->GetGraphicsKey();
    if (std::popcount(key.mrt_mask & 0x7f) >= 5 && db_desc.first &&
        scene_targets->EligibleScene(texture_cache.GetImage(db_desc.first))) scene_started = true;
    // A proxy attachment cannot represent MSAA or a feedback loop that reads the
    // same image through the guest's native descriptor during this draw.
    bool reduced = scene_started && upscaler->RasterScaling() && key.num_samples == 1;
    // GOW3_SCENE_DEBUG=<file>: once the file exists, why the passes of one frame keep the
    // native size (see NoteFrameStart).
    const bool debug_pass = scene_debug_frame && scene_started;
    std::string why;
    if (debug_pass) {
        why = fmt::format("pass vs {:08x} ps {:08x} mrt {:#x} samples {} raster_scaling {}:",
                          pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash,
                          key.mrt_mask ? pipeline->GetStage(Shader::SwStage::Fragment).pgm_hash
                                       : 0,
                          key.mrt_mask, u32(key.num_samples), upscaler->RasterScaling());
    }
    // Only this pass's attachments: slots past the mask's width keep earlier passes' targets,
    // often the G-buffer images the lighting passes sample (bound), which kept those native.
    const u32 num_attachments = Gow3Toggle::Disabled(Gow3Toggle::SceneAttachmentsOnly)
                                    ? u32(cb_descs.size())
                                    : u32(std::bit_width(key.mrt_mask));
    // All attachments of a reduced pass share one proxy size (scene or half resolution).
    std::optional<SceneResolution::Size> pass_size;
    const auto same_size = [&](const VideoCore::Image& image, u32 level) {
        const auto proxy = scene_targets->ProxySize(image, level);
        if (!pass_size) pass_size = proxy;
        return *pass_size == proxy;
    };
    for (u32 cb = 0; cb < num_attachments; ++cb) {
        const auto& [id, desc] = cb_descs[cb];
        if (id) {
            const auto& image = texture_cache.GetImage(id);
            // Mip levels of eligible chains have their own proxies (SceneTargets::Get).
            if (!scene_targets->Eligible(image) || image.binding.is_bound ||
                image.binding.needs_rebind || desc.view_info.range.base.layer ||
                !same_size(image, desc.view_info.range.base.level)) reduced = false;
            if (debug_pass) {
                why += fmt::format(" [{} {}x{} eligible {} bound {} rebind {} level {} layer {}]",
                                   vk::to_string(image.info.pixel_format), image.info.size.width,
                                   image.info.size.height, scene_targets->Eligible(image),
                                   u32(image.binding.is_bound), u32(image.binding.needs_rebind),
                                   desc.view_info.range.base.level,
                                   desc.view_info.range.base.layer);
            }
        }
    }
    if (db_desc.first && (!scene_targets->Eligible(texture_cache.GetImage(db_desc.first)) ||
                          !same_size(texture_cache.GetImage(db_desc.first), 0))) {
        reduced = false;
    }
    if (debug_pass) {
        if (db_desc.first) {
            const auto& depth = texture_cache.GetImage(db_desc.first);
            why += fmt::format(" depth [{} {}x{} eligible {}]",
                               vk::to_string(depth.info.pixel_format), depth.info.size.width,
                               depth.info.size.height, scene_targets->Eligible(depth));
        }
        std::printf("Scene pass: reduced %d %s\n", reduced, why.c_str());
    }
    push_data.scene_size = reduced ? SceneResolution::Pack(scene_targets->Size()) : 0;
    if (Gow3Stats::enabled) {
        Gow3Stats::reduced_draws.fetch_add(reduced, std::memory_order_relaxed);
        Gow3Stats::scene_draws.fetch_add(scene_started, std::memory_order_relaxed);
    }
    RenderState state;
    state.width = instance.GetMaxFramebufferWidth();
    state.height = instance.GetMaxFramebufferHeight();
    state.num_layers = std::numeric_limits<u16>::max();
    state.num_color_attachments = std::bit_width(key.mrt_mask);
    for (auto cb = 0u; cb < state.num_color_attachments; ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        if (!image_id) {
            state.color_attachments[cb] = {};
            continue;
        }
        auto* image = &texture_cache.GetImage(image_id);
        if (image->binding.needs_rebind) {
            last_targets[cb].generation = ~0ULL; // FindImage may rewrite the description
            image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
            image = &texture_cache.GetImage(image_id);
        }
        texture_cache.UpdateImage(image_id);
        runtime.SetBackingSamples(image, key.color_samples[cb]);
        const auto& image_view = texture_cache.FindRenderTarget(image_id, desc);
        const auto slice = image_view.info.range.base.layer;
        const auto mip = image_view.info.range.base.level;

        const auto& col_buf = regs.color_buffers[cb];
        const bool is_clear = texture_cache.IsMetaCleared(col_buf.CmaskAddress(), slice);
        texture_cache.TouchMeta(col_buf.CmaskAddress(), slice, false);

        if (reduced) {
            // The native image is resolved lazily when a shader/copy actually reads it.
        } else if (image->binding.is_bound) {
            ASSERT_MSG(!image->binding.force_general,
                       "Having image both as storage and render target is unsupported");
            runtime.FlushBarriers();
            if (FeedbackTraceFirst((image->image_uid << 16) | (u64(desc.view_info.range.base.level) << 8) |
                                   (u64(desc.view_info.range.extent.levels) << 1) | 1)) {
                std::printf("MIP_FIX trace: render target also sampled: %#llx %ux%u levels %u, target mip %u+%u\n",
                            (unsigned long long)image->info.guest_address, image->info.size.width,
                            image->info.size.height, u32(image->info.resources.levels),
                            u32(desc.view_info.range.base.level), u32(desc.view_info.range.extent.levels));
            }
            needs_barrier |=
                runtime.Transit(image,
                                instance.IsAttachmentFeedbackLoopLayoutSupported()
                                    ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
                                    : vk::ImageLayout::eGeneral,
                                vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                vk::AccessFlagBits2::eColorAttachmentWrite |
                                    vk::AccessFlagBits2::eColorAttachmentRead);
            attachment_feedback_loop = true;
        } else {
            needs_barrier |= runtime.Transit(image, vk::ImageLayout::eColorAttachmentOptimal,
                                             vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                             vk::AccessFlagBits2::eColorAttachmentWrite |
                                                 vk::AccessFlagBits2::eColorAttachmentRead,
                                             desc.view_info.range);
        }

        state.width = std::min<u32>(state.width, std::max(image->info.size.width >> mip, 1u));
        state.height = std::min<u32>(state.height, std::max(image->info.size.height >> mip, 1u));
        state.num_layers = std::min<u32>(state.num_layers, image_view.info.range.extent.layers);

        const auto clear_value =
            is_clear ? LiverpoolToVK::ColorBufferClearValue(col_buf) : vk::ClearValue{};
        auto& attachment = state.color_attachments[cb];
        attachment.image_view = *image_view.image_view;
        attachment.image_layout = image->backing->state.layout;
        attachment.clear_value = clear_value.color.uint32;
        attachment.is_clear = is_clear;

        if (reduced) {
            const auto target = scene_targets->Attachment(image_id, desc.view_info);
            attachment.image_view = target.view;
            attachment.image_layout = target.layout;
        }
        image->usage.render_target = 1u;
        if (cb == 0 && upscaler->Enabled()) {
            color_redirected = upscaler->RedirectColor(image_id, desc.view_info, redirect);
            if (color_redirected) {
                guest_extent = {image->info.size.width, image->info.size.height};
                original_color = {attachment.image_view, attachment.image_layout};
                attachment.image_view = redirect.view;
                attachment.image_layout = redirect.layout;
            }
        }
    }
    for (u32 cb = state.num_color_attachments; cb < state.color_attachments.size(); ++cb) {
        state.color_attachments[cb] = {};
    }

    if (auto image_id = db_desc.first; image_id) {
        auto& desc = db_desc.second;
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        const auto& image_view = texture_cache.FindDepthTarget(image_id, desc);
        auto& image = texture_cache.GetImage(image_id);

        const auto slice = image_view.info.range.base.layer;
        const bool is_depth_clear =
            (regs.depth_render_control.depth_clear_enable && regs.depth_control.depth_enable &&
             regs.depth_control.depth_write_enable) ||
            texture_cache.IsMetaCleared(htile_address, slice);
        const bool is_stencil_clear = regs.depth_render_control.stencil_clear_enable;
        texture_cache.TouchMeta(htile_address, slice, false);
        ASSERT(desc.view_info.range.extent.levels == 1 && !image.binding.needs_rebind);

        const bool has_stencil = image.info.props.has_stencil;
        // Stencil writes can be enabled while depth writes are off.
        const bool stencil_write =
            has_stencil && regs.depth_control.stencil_enable && !desc.view_info.is_storage;
        const auto new_layout = desc.view_info.is_storage
                                    ? has_stencil ? vk::ImageLayout::eDepthStencilAttachmentOptimal
                                                  : vk::ImageLayout::eDepthAttachmentOptimal
                                : stencil_write
                                    ? vk::ImageLayout::eDepthReadOnlyStencilAttachmentOptimal
                                : has_stencil ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                              : vk::ImageLayout::eDepthReadOnlyOptimal;
        if (!reduced) needs_barrier |= runtime.Transit(&image, new_layout,
                                         vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                             vk::PipelineStageFlagBits2::eLateFragmentTests,
                                         vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
                                             vk::AccessFlagBits2::eDepthStencilAttachmentRead,
                                         desc.view_info.range);

        state.width = std::min<u32>(state.width, image.info.size.width);
        state.height = std::min<u32>(state.height, image.info.size.height);
        state.num_layers = std::min<u32>(state.num_layers, image_view.info.range.extent.layers);

        auto& attachment = state.depth_stencil_attachment;
        attachment.image_view = *image_view.image_view;
        attachment.image_layout = image.backing->state.layout;
        attachment.clear_value = {};
        attachment.is_clear = 0;

        if (regs.depth_buffer.DepthValid()) {
            attachment.clear_value[0] = is_depth_clear ? std::bit_cast<u32>(regs.depth_clear) : 0u;
            attachment.has_depth = true;
            attachment.depth_clear = is_depth_clear;
        }
        if (regs.depth_buffer.StencilValid()) {
            attachment.clear_value[1] = is_stencil_clear ? regs.stencil_clear : 0u;
            attachment.has_stencil = true;
            attachment.stencil_clear = is_stencil_clear;
        }

        if (reduced) {
            const auto target = scene_targets->Attachment(image_id, desc.view_info);
            attachment.image_view = target.view;
            attachment.image_layout = target.layout;
        }
        image.usage.depth_target = true;
        if (upscaler->Enabled()) {
            VulkanUpscalerTarget depth_redirect;
            depth_redirected = upscaler->RedirectDepth(image_id, depth_redirect);
            if (depth_redirected) {
                guest_extent = {image.info.size.width, image.info.size.height};
                attachment.image_view = depth_redirect.view;
                attachment.image_layout = depth_redirect.layout;
                redirect = depth_redirect;
            }
        }
    } else {
        state.depth_stencil_attachment = {};
    }

    // gow3: a pass drawn into the upscaler's output-size images (UI, display pass): every
    // attachment must be redirected, viewports and scissors are scaled.
    target_scale = {1.0f, 1.0f};
    if (color_redirected || depth_redirected) {
        u32 color_targets = 0;
        for (u32 cb = 0; cb < state.num_color_attachments; ++cb) {
            color_targets += state.color_attachments[cb].image_view ? 1 : 0;
        }
        const bool has_depth = bool(db_desc.first);
        if ((color_targets > 0 && !color_redirected) || (has_depth && !depth_redirected) ||
            color_targets > 1) {
            static u32 warned = 0;
            if (warned < 8) {
                ++warned;
                const auto describe = [&](VideoCore::ImageId id) {
                    if (!id) {
                        return std::string{"-"};
                    }
                    const auto& info = texture_cache.GetImage(id).info;
                    return fmt::format("{:#x} {}x{} {}", info.guest_address, info.size.width,
                                       info.size.height, vk::to_string(info.pixel_format));
                };
                std::printf("Upscaler: pass not redirected (color %s: %s, depth %s: %s, %u "
                            "targets)\n",
                            color_redirected ? "yes" : "no", describe(cb_descs[0].first).c_str(),
                            depth_redirected ? "yes" : "no", describe(db_desc.first).c_str(),
                            state.num_color_attachments);
            }
            if (color_redirected) {
                state.color_attachments[0].image_view = original_color.first;
                state.color_attachments[0].image_layout = original_color.second;
            }
            if (depth_redirected) {
                auto& depth_image = texture_cache.GetImage(db_desc.first);
                state.depth_stencil_attachment.image_view =
                    *texture_cache.FindDepthTarget(db_desc.first, db_desc.second).image_view;
                state.depth_stencil_attachment.image_layout = depth_image.backing->state.layout;
            }
        } else {
            state.width = redirect.width;
            state.height = redirect.height;
            // The UI movie viewport is now 1920x1080 even when the scene is 960x540.
            // A native viewport (or native window-space UI vertices) must not be doubled
            // again. The final display copy still uses the guest render-size coordinates.
            const auto& viewport = regs.viewports[0];
            const bool native_coordinates = redirect.native_ui &&
                (UiComposition::NativeViewport(viewport.xscale * 2, viewport.yscale * 2) ||
                 (regs.IsClipDisabled() && !regs.viewport_control.xscale_enable &&
                  !regs.viewport_control.yscale_enable));
            target_scale = UiComposition::Scale(guest_extent.first, guest_extent.second,
                                                redirect.width, redirect.height,
                                                native_coordinates);
        }
    }

    if (state.num_layers == std::numeric_limits<u16>::max()) {
        state.num_layers = 1;
    }

    if (reduced) {
        // Half-resolution passes keep the scene's factor (proxy / native = scene / 1920).
        const auto size = scene_targets->Size();
        const auto proxy = pass_size.value_or(size);
        state.width = proxy.width;
        state.height = proxy.height;
        target_scale = {float(size.width) / 1920.0f, float(size.height) / 1080.0f};
    }
    if (FrameCapture::Active()) {
        std::array<const VideoCore::ImageInfo*, AmdGpu::NUM_COLOR_BUFFERS> colors{};
        for (u32 cb = 0; cb < state.num_color_attachments; ++cb) {
            if (cb_descs[cb].first) {
                colors[cb] = &texture_cache.GetImage(cb_descs[cb].first).info;
            }
        }
        const auto* depth =
            db_desc.first ? &texture_cache.GetImage(db_desc.first).info : nullptr;
        FrameCapture::BeginPass(colors.data(), state.num_color_attachments, depth);
    }
    // gow3: object motion vector attachment of G-buffer pipelines.
    if (key.motion_vectors) {
        object_motion->Attach(state, state.width, state.height);
    }
    return state;
}

void Rasterizer::Resolve() {
    const auto& mrt0_hint = CbExtent(0);
    const auto& mrt1_hint = CbExtent(1);
    VideoCore::TextureCache::ImageDesc mrt0_desc{Regs().color_buffers[0], mrt0_hint};
    VideoCore::TextureCache::ImageDesc mrt1_desc{Regs().color_buffers[1], mrt1_hint};
    auto& mrt0_image = texture_cache.GetImage(texture_cache.FindImage(mrt0_desc, true));
    auto& mrt1_image = texture_cache.GetImage(texture_cache.FindImage(mrt1_desc, true));

    ScopeMarkerBegin(fmt::format("Resolve:MRT0={:#x}:MRT1={:#x}",
                                 Regs().color_buffers[0].Address(),
                                 Regs().color_buffers[1].Address()));
    runtime.ResolveImage(&mrt0_image, &mrt1_image, mrt0_desc.view_info.range,
                         mrt1_desc.view_info.range);
    ScopeMarkerEnd();
}

void Rasterizer::DepthStencilCopy(bool is_depth, bool is_stencil) {
    auto& regs = Regs();

    auto read_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), DbExtent(), false);
    auto write_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), DbExtent(), true);

    auto& read_image = texture_cache.GetImage(texture_cache.FindImage(read_desc));
    const auto write_id = texture_cache.FindImage(write_desc);
    auto& write_image = texture_cache.GetImage(write_id);
    // The copy destination is never bound as a target, so its stencil plane is linked here.
    texture_cache.AssociateStencil(write_id, write_desc.info);

    VideoCore::SubresourceRange sub_range;
    sub_range.base.layer = Regs().depth_view.slice_start;
    sub_range.extent.layers = Regs().depth_view.NumSlices() - sub_range.base.layer;

    ScopeMarkerBegin(fmt::format(
        "DepthStencilCopy:DR={:#x}:SR={:#x}:DW={:#x}:SW={:#x}", regs.depth_buffer.DepthAddress(),
        regs.depth_buffer.StencilAddress(), regs.depth_buffer.DepthWriteAddress(),
        regs.depth_buffer.StencilWriteAddress()));

    runtime.CopyDepthStencil(&read_image, &write_image, sub_range);

    ScopeMarkerEnd();
}

void Rasterizer::FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds) {
    DrainDrawPipe();
    ASSERT_MSG(address % 4 == 0 && num_bytes % 4 == 0,
               "FillBuffer address and size must be a multiple of 4 bytes");
    if (!is_gds) {
        texture_cache.ClearMeta(address);
        const bool gpu_page = buffer_cache.IsRegionGpuModified(address, num_bytes);
        if (Gow3Stats::d4_diag) {
            // Fable R.14 D4 diagnostics: which branch small immediate fills take, and why.
            (gpu_page ? Gow3Stats::fill_gpu : Gow3Stats::fill_cpu)
                .fetch_add(1, std::memory_order_relaxed);
            if (gpu_page) {
                const bool own = buffer_cache.IsGpuModifiedBytes(address, num_bytes);
                Gow3Stats::fill_gpu_own_bytes.fetch_add(own ? 1 : 0, std::memory_order_relaxed);
                static std::atomic<u32> printed{0};
                if (printed.fetch_add(1, std::memory_order_relaxed) < 12) {
                    std::printf("uc3 D4 fill on GPU: %010llx +%u value %08x, own bytes %s; page "
                                "ranges%s\n",
                                static_cast<unsigned long long>(address), num_bytes, value,
                                own ? "GPU" : "clean",
                                buffer_cache.DescribeGpuRanges(address & ~VAddr{4095}, 4096)
                                    .c_str());
                }
            }
        }
        if (!gpu_page) {
            u32* buffer = std::bit_cast<u32*>(address);
            std::fill(buffer, buffer + (num_bytes / sizeof(u32)), value);
            return;
        }
    }
    const auto [buffer, offset] = [&] -> std::pair<const VideoCore::Buffer*, u64> {
        if (is_gds) {
            return {buffer_cache.GetGdsBuffer(), address};
        }
        return buffer_cache.ObtainBuffer(address, num_bytes, true);
    }();
    runtime.FillBuffer(buffer, offset, num_bytes, value);
}

void Rasterizer::CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds) {
    // uc3: GOW3_WATCH_ADDR (see texture_cache.cpp): DMA copies into the watched address.
    static const VAddr watch_addr = [] {
        const char* env = std::getenv("GOW3_WATCH_ADDR");
        return env ? VAddr(std::strtoull(env, nullptr, 16)) : VAddr(0);
    }();
    if (watch_addr && !dst_gds && dst < watch_addr + 0x20000 && dst + num_bytes > watch_addr) {
        std::printf("ucndc: watch dma copy dst %#llx src %#llx bytes %#x src_gds %d\n",
                    (unsigned long long)dst, (unsigned long long)src, num_bytes, int(src_gds));
        std::fflush(stdout);
    }
    DrainDrawPipe();
    if (!dst_gds && !buffer_cache.IsRegionGpuModified(dst, num_bytes)) {
        if (!src_gds && !buffer_cache.IsRegionGpuModified(src, num_bytes) &&
            !texture_cache.FindImageFromRange(src, num_bytes)) {
            // Both buffers were not transferred to GPU yet. Can safely copy in host memory.
            NoteGuestWrite(dst, num_bytes);
            std::memcpy(std::bit_cast<void*>(dst), std::bit_cast<void*>(src), num_bytes);
            SrtNoteWrite(dst, num_bytes, SrtOriginRewrite); // uc3 R.21: again once written
            return;
        }
    }
    texture_cache.InvalidateMemoryFromGPU(dst, num_bytes);
    const auto* gds_buffer = buffer_cache.GetGdsBuffer();
    const auto [src_buffer, src_offset] = [&] -> std::pair<const VideoCore::Buffer*, u64> {
        if (src_gds) {
            return {gds_buffer, src};
        }
        return buffer_cache.ObtainBuffer(src, num_bytes, false, true);
    }();
    const auto [dst_buffer, dst_offset] = [&] -> std::pair<const VideoCore::Buffer*, u64> {
        if (dst_gds) {
            return {gds_buffer, dst};
        }
        return buffer_cache.ObtainBuffer(dst, num_bytes, true, true);
    }();
    const vk::BufferCopy copy = {
        .srcOffset = src_offset,
        .dstOffset = dst_offset,
        .size = num_bytes,
    };
    runtime.CopyBuffer(src_buffer, dst_buffer, std::span{&copy, 1});
}

u32 Rasterizer::ReadDataFromGds(u32 gds_offset) {
    DrainDrawPipe();
    auto* gds_buf = buffer_cache.GetGdsBuffer();
    u32 value;
    std::memcpy(&value, gds_buf->mapped_data.data() + gds_offset, sizeof(u32));
    return value;
}

bool Rasterizer::InvalidateMemory(VAddr addr, u64 size, bool assume_locks, bool from_fault) {
    DrainDrawPipe();
    if (!IsMapped(addr, size)) {
        // Not GPU mapped memory, can skip invalidation logic entirely.
        return false;
    }
    texture_cache.ResolveReadbacks(addr, size, assume_locks, VideoCore::ReadbackSource::CpuWrite);
    buffer_cache.InvalidateMemory(addr, size, assume_locks, from_fault);
    texture_cache.InvalidateMemory(addr, size);
    return true;
}

bool Rasterizer::OnWriteFault(VAddr addr, bool assume_locks) {
    DrainDrawPipe();
    if (!InvalidateMemory(addr, 8, assume_locks, true)) {
        return false;
    }
    buffer_cache.ExtendWriteFault(addr);
    return true;
}

bool Rasterizer::HasImageReadbackBytes(VAddr addr, u64 size) {
    return texture_cache.HasPendingReadbackBytes(addr, size);
}

void Rasterizer::RecordLeafCopies() {
    if (!constant_ring) {
        return;
    }
    const auto* ring_buffer = constant_ring->Object();
    for (u32 i = 0; i < num_ring_stages; ++i) {
        const auto& stage = ring_stages[i];
        for (u32 j = 0; j < stage.num_leaves; ++j) {
            const auto& leaf = stage.leaves[j];
            const auto [buffer, offset] = buffer_cache.ObtainBuffer(leaf.src, sizeof(u32), false);
            const vk::BufferCopy copy{offset, leaf.ring_offset, sizeof(u32)};
            runtime.CopyBuffer(buffer, ring_buffer, {&copy, 1});
            Gow3Stats::uc3_leaf_copies.fetch_add(1, std::memory_order_relaxed);
            buffer_cache.QueueLeafDownload(buffer, offset, leaf.src, sizeof(u32));
        }
    }
}

bool Rasterizer::IsGpuWritten(VAddr addr, u64 size) {
    return buffer_cache.IsGpuWritten(addr, size) ||
           texture_cache.HasPendingReadbackBytes(addr, size);
}

const char* Rasterizer::DescribeGpuWritten(VAddr addr, u64 size) {
    return buffer_cache.DescribeGpuWritten(addr, size);
}

bool Rasterizer::ReadMemory(VAddr addr, u64 size, bool assume_locks) {
    DrainDrawPipe();
    if (!IsMapped(addr, size)) {
        // Not GPU mapped memory, can skip invalidation logic entirely.
        return false;
    }
    const bool image_readback =
        texture_cache.ResolveReadbacks(addr, size, assume_locks, VideoCore::ReadbackSource::CpuRead);
    if (!image_readback || buffer_cache.IsRegionGpuModified(addr, size)) {
        buffer_cache.ReadMemory(addr, size, false, assume_locks, true);
    }
    return true;
}

void Rasterizer::ProcessDownloadImages() {
    DrainDrawPipe();
    texture_cache.ProcessDownloadImages();
}

bool Rasterizer::IsMapped(VAddr addr, u64 size) {
    if (size == 0) {
        // There is no memory, so not mapped.
        return false;
    }
    if (static_cast<u64>(addr) > std::numeric_limits<u64>::max() - size) {
        // Memory range wrapped the address space, cannot be mapped.
        return false;
    }
    const auto range = decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);

    Common::RecursiveSharedLock lock{mapped_ranges_mutex};
    return boost::icl::contains(mapped_ranges, range);
}

void Rasterizer::MapMemory(VAddr addr, u64 size) {
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_ranges += decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
    }
}

void Rasterizer::RegisterMemory(VAddr addr, u64 size) {
    page_manager.OnGpuMap(addr, size);
}

void Rasterizer::UnmapMemory(VAddr addr, u64 size) {
    DrainDrawPipe();
    buffer_cache.InvalidateMemory(addr, size);
    texture_cache.UnmapMemory(addr, size);
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_ranges -= decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
    }
}

void Rasterizer::UpdateDynamicState(const GraphicsPipeline* pipeline, const bool is_indexed) const {
    UpdateViewportScissorState();
    UpdateDepthStencilState();
    UpdatePrimitiveState(is_indexed);
    UpdateRasterizationState();
    UpdateColorBlendingState(pipeline);

    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.CommitWith(instance.IsDepthBoundsSupported(),
                             instance.IsDynamicColorWriteMaskSupported(),
                             instance.IsAttachmentFeedbackLoopLayoutSupported(),
                             [&](auto&& command) { scheduler.Record(std::move(command)); });
}

void Rasterizer::UpdateViewportScissorState() const {
    const auto& regs = Regs();

    const auto combined_scissor_value_tl = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::max({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const auto combined_scissor_value_br = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::min({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const bool enable_offset = !regs.window_scissor.window_offset_disable;

    AmdGpu::Scissor scsr{};
    scsr.top_left_x = combined_scissor_value_tl(
        regs.screen_scissor.top_left_x, s16(regs.window_scissor.top_left_x),
        s16(regs.generic_scissor.top_left_x),
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.top_left_y = combined_scissor_value_tl(
        regs.screen_scissor.top_left_y, s16(regs.window_scissor.top_left_y),
        s16(regs.generic_scissor.top_left_y),
        enable_offset ? regs.window_offset.window_y_offset : 0);
    scsr.bottom_right_x = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_x, regs.window_scissor.bottom_right_x,
        regs.generic_scissor.bottom_right_x,
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.bottom_right_y = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_y, regs.window_scissor.bottom_right_y,
        regs.generic_scissor.bottom_right_y,
        enable_offset ? regs.window_offset.window_y_offset : 0);

    boost::container::static_vector<vk::Viewport, AmdGpu::NUM_VIEWPORTS> viewports;
    boost::container::static_vector<vk::Rect2D, AmdGpu::NUM_VIEWPORTS> scissors;

    if (regs.polygon_control.enable_window_offset &&
        (regs.window_offset.window_x_offset != 0 || regs.window_offset.window_y_offset != 0)) {
        LOG_ERROR(Render_Vulkan,
                  "PA_SU_SC_MODE_CNTL.VTX_WINDOW_OFFSET_ENABLE support is not yet implemented.");
    }

    const auto& vp_ctl = regs.viewport_control;
    for (u32 i = 0; i < AmdGpu::NUM_VIEWPORTS; i++) {
        const auto& vp = regs.viewports[i];
        const auto& vp_d = regs.viewport_depths[i];
        if (vp.xscale == 0) {
            continue;
        }

        const auto zoffset = vp_ctl.zoffset_enable ? vp.zoffset : 0.f;
        const auto zscale = vp_ctl.zscale_enable ? vp.zscale : 1.f;

        vk::Viewport viewport{};

        // https://gitlab.freedesktop.org/mesa/mesa/-/blob/209a0ed/src/amd/vulkan/radv_pipeline_graphics.c#L688-689
        // https://gitlab.freedesktop.org/mesa/mesa/-/blob/209a0ed/src/amd/vulkan/radv_cmd_buffer.c#L3103-3109
        // When the clip space is ranged [-1...1], the zoffset is centered.
        // By reversing the above viewport calculations, we get the following:
        if (regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW) {
            viewport.minDepth = zoffset - zscale;
            viewport.maxDepth = zoffset + zscale;
        } else {
            viewport.minDepth = zoffset;
            viewport.maxDepth = zoffset + zscale;
        }

        if (!instance.IsDepthRangeUnrestrictedSupported()) {
            // Unrestricted depth range not supported by device. Restrict to valid range.
            viewport.minDepth = std::max(viewport.minDepth, 0.f);
            viewport.maxDepth = std::min(viewport.maxDepth, 1.f);
        }

        if (regs.IsClipDisabled()) {
            // In case if clipping is disabled we patch the shader to convert vertex position
            // from screen space coordinates to NDC by defining a render space as full hardware
            // window range [0..16383, 0..16383] and setting the viewport to its size.
            viewport.x = 0.f;
            viewport.y = 0.f;
            viewport.width = float(std::min<u32>(instance.GetMaxViewportWidth(), 16_KB));
            viewport.height = float(std::min<u32>(instance.GetMaxViewportHeight(), 16_KB));
        } else {
            const auto xoffset = vp_ctl.xoffset_enable ? vp.xoffset : 0.f;
            const auto xscale = vp_ctl.xscale_enable ? vp.xscale : 1.f;
            const auto yoffset = vp_ctl.yoffset_enable ? vp.yoffset : 0.f;
            const auto yscale = vp_ctl.yscale_enable ? vp.yscale : 1.f;

            // gow3: sub-pixel jitter of scene geometry for the temporal upscaler; the same
            // shift as jittering the projection.
            viewport.x = (xoffset - xscale) * target_scale[0] + draw_jitter[0];
            viewport.y = (yoffset - yscale) * target_scale[1] + draw_jitter[1];
            viewport.width = xscale * 2.0f * target_scale[0];
            viewport.height = yscale * 2.0f * target_scale[1];
        }

        viewports.push_back(viewport);

        auto vp_scsr = scsr;
        if (regs.mode_control.vport_scissor_enable) {
            vp_scsr.top_left_x =
                std::max(vp_scsr.top_left_x, s16(regs.viewport_scissors[i].top_left_x));
            vp_scsr.top_left_y =
                std::max(vp_scsr.top_left_y, s16(regs.viewport_scissors[i].top_left_y));
            vp_scsr.bottom_right_x = std::min(AmdGpu::Scissor::Clamp(vp_scsr.bottom_right_x),
                                              regs.viewport_scissors[i].bottom_right_x);
            vp_scsr.bottom_right_y = std::min(AmdGpu::Scissor::Clamp(vp_scsr.bottom_right_y),
                                              regs.viewport_scissors[i].bottom_right_y);
        }
        if (target_scale[0] != 1.0f || target_scale[1] != 1.0f) {
            const auto scale = [](s32 v, float f) { return s32(std::lround(float(v) * f)); };
            const s32 x0 = scale(vp_scsr.top_left_x, target_scale[0]);
            const s32 y0 = scale(vp_scsr.top_left_y, target_scale[1]);
            const s32 x1 = scale(vp_scsr.top_left_x + s32(vp_scsr.GetWidth()), target_scale[0]);
            const s32 y1 = scale(vp_scsr.top_left_y + s32(vp_scsr.GetHeight()), target_scale[1]);
            scissors.push_back({
                .offset = {x0, y0},
                .extent = {u32(std::max(x1 - x0, 0)), u32(std::max(y1 - y0, 0))},
            });
        } else {
            scissors.push_back({
                .offset = {vp_scsr.top_left_x, vp_scsr.top_left_y},
                .extent = {vp_scsr.GetWidth(), vp_scsr.GetHeight()},
            });
        }
    }

    if (viewports.empty()) {
        // Vulkan requires providing at least one viewport.
        constexpr vk::Viewport empty_viewport = {
            .x = -1.0f,
            .y = -1.0f,
            .width = 1.0f,
            .height = 1.0f,
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        constexpr vk::Rect2D empty_scissor = {
            .offset = {0, 0},
            .extent = {1, 1},
        };
        viewports.push_back(empty_viewport);
        scissors.push_back(empty_scissor);
    }

    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetViewports(viewports);
    dynamic_state.SetScissors(scissors);
}

vk::Pipeline Rasterizer::SquarePass(const GraphicsPipeline& pipeline) const {
    const vk::Pipeline square = pipeline.SquarePassHandle();
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_SQUARE_PASS");
        return !env || env[0] != '0';
    }();
    if (!square || !enabled) [[likely]] {
        return {};
    }
    // MIN(s * s, d * d) = MIN(s, d)^2: the second draw needs the first one's coverage, which a
    // depth or stencil write would change.
    const auto& regs = Regs();
    const bool depth_write = regs.depth_control.depth_enable &&
                             regs.depth_control.depth_write_enable && regs.depth_buffer.DepthValid();
    if (depth_write || regs.depth_control.stencil_enable) {
        static bool warned = false;
        if (!std::exchange(warned, true)) {
            LOG_WARNING(Render_Vulkan, "Scaled MIN/MAX blend not squared: the draw writes depth "
                                       "or stencil");
        }
        return {};
    }
    return square;
}

void Rasterizer::UpdateDepthStencilState() const {
    const auto& regs = Regs();
    auto& dynamic_state = scheduler.GetDynamicState();

    const auto depth_stencil = GetEffectiveDepthStencilState(regs);
    const bool depth_test_enabled = depth_stencil.depth_test_enable;
    dynamic_state.SetDepthTestEnabled(depth_test_enabled);
    dynamic_state.SetDepthWriteEnabled(depth_stencil.depth_write_enable &&
                                       !regs.depth_render_control.depth_clear_enable);
    if (depth_test_enabled) {
        dynamic_state.SetDepthCompareOp(LiverpoolToVK::CompareOp(regs.depth_control.depth_func));
    }

    const bool depth_bounds_test_enabled = depth_stencil.depth_bounds_enable;
    dynamic_state.SetDepthBoundsTestEnabled(depth_bounds_test_enabled);
    if (depth_bounds_test_enabled) {
        dynamic_state.SetDepthBounds(regs.depth_bounds_min, regs.depth_bounds_max);
    }

    const auto depth_bias_enabled = regs.polygon_control.NeedsBias();
    dynamic_state.SetDepthBiasEnabled(depth_bias_enabled);
    if (depth_bias_enabled) {
        const bool front = regs.polygon_control.enable_polygon_offset_front;
        dynamic_state.SetDepthBias(
            front ? regs.poly_offset.front_offset : regs.poly_offset.back_offset,
            regs.poly_offset.depth_bias,
            (front ? regs.poly_offset.front_scale : regs.poly_offset.back_scale) / 16.f);
    }

    const bool stencil_test_enabled = depth_stencil.stencil_test_enable;
    dynamic_state.SetStencilTestEnabled(stencil_test_enabled);
    if (stencil_test_enabled) {
        const StencilOps front_ops{
            .fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_front),
            .pass_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_front),
            .depth_fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_front),
            .compare_op = LiverpoolToVK::CompareOp(regs.depth_control.stencil_ref_func),
        };
        const StencilOps back_ops = regs.depth_control.backface_enable ? StencilOps{
            .fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_back),
            .pass_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_back),
            .depth_fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_back),
            .compare_op = LiverpoolToVK::CompareOp(regs.depth_control.stencil_bf_func),
        } : front_ops;
        dynamic_state.SetStencilOps(front_ops, back_ops);

        const bool stencil_clear = regs.depth_render_control.stencil_clear_enable;
        const auto front = regs.stencil_ref_front;
        const auto back =
            regs.depth_control.backface_enable ? regs.stencil_ref_back : regs.stencil_ref_front;
        // GCN REPLACE_OP writes DB_STENCILREFMASK.STENCILOPVAL, so a face whose stencil ops
        // include ReplaceOp takes its Vulkan reference from op_val.
        const auto& sc = regs.stencil_control;
        const auto uses_op_val = [](AmdGpu::StencilFunc fail, AmdGpu::StencilFunc zpass,
                                    AmdGpu::StencilFunc zfail) {
            return fail == AmdGpu::StencilFunc::ReplaceOp ||
                   zpass == AmdGpu::StencilFunc::ReplaceOp ||
                   zfail == AmdGpu::StencilFunc::ReplaceOp;
        };
        const bool front_op =
            uses_op_val(sc.stencil_fail_front, sc.stencil_zpass_front, sc.stencil_zfail_front);
        const bool back_op =
            regs.depth_control.backface_enable
                ? uses_op_val(sc.stencil_fail_back, sc.stencil_zpass_back, sc.stencil_zfail_back)
                : front_op;
        const auto ref_conflict = [](AmdGpu::CompareFunc func, const AmdGpu::StencilRefMask& ref) {
            return func != AmdGpu::CompareFunc::Always && func != AmdGpu::CompareFunc::Never &&
                   ref.stencil_test_val != ref.stencil_op_val;
        };
        if ((front_op && ref_conflict(regs.depth_control.stencil_ref_func, front)) ||
            (back_op && regs.depth_control.backface_enable &&
             ref_conflict(regs.depth_control.stencil_bf_func, back))) {
            LOG_WARNING(Render_Vulkan, "Stencil test requires test_val while ReplaceOp requires "
                                       "op_val; the stencil test will use op_val");
        }
        dynamic_state.SetStencilReferences(front_op ? front.stencil_op_val : front.stencil_test_val,
                                           back_op ? back.stencil_op_val : back.stencil_test_val);
        dynamic_state.SetStencilWriteMasks(!stencil_clear ? front.stencil_write_mask : 0U,
                                           !stencil_clear ? back.stencil_write_mask : 0U);
        dynamic_state.SetStencilCompareMasks(front.stencil_mask, back.stencil_mask);
    }
}

void Rasterizer::UpdatePrimitiveState(const bool is_indexed) const {
    const auto& regs = Regs();
    auto& dynamic_state = scheduler.GetDynamicState();

    const auto is_list_topology = [](const AmdGpu::PrimitiveType type) {
        const auto topology = LiverpoolToVK::PrimitiveType(type);
        return topology == vk::PrimitiveTopology::ePointList ||
               topology == vk::PrimitiveTopology::eLineList ||
               topology == vk::PrimitiveTopology::eTriangleList ||
               topology == vk::PrimitiveTopology::eLineListWithAdjacency ||
               topology == vk::PrimitiveTopology::eTriangleListWithAdjacency;
    };
    const auto is_patch_list_topology = [](const AmdGpu::PrimitiveType type) {
        // Quad and rect lists are emulated using tessellation.
        return type == AmdGpu::PrimitiveType::PatchPrimitive ||
               type == AmdGpu::PrimitiveType::QuadList || type == AmdGpu::PrimitiveType::RectList;
    };

    const auto prim_restart =
        (regs.enable_primitive_restart & 1) != 0 &&
        (instance.IsListRestartSupported() || !is_list_topology(regs.primitive_type)) &&
        (instance.IsPatchListRestartSupported() || !is_patch_list_topology(regs.primitive_type));
    ASSERT_MSG(!is_indexed || !prim_restart || regs.primitive_restart_index == 0xFFFF ||
                   regs.primitive_restart_index == 0xFFFFFFFF,
               "Primitive restart index other than -1 is not supported yet");

    const auto cull_mode = LiverpoolToVK::IsPrimitiveCulled(regs.primitive_type)
                               ? LiverpoolToVK::CullMode(regs.polygon_control.CullingMode())
                               : vk::CullModeFlagBits::eNone;
    const auto front_face = LiverpoolToVK::FrontFace(regs.polygon_control.front_face);

    dynamic_state.SetPrimitiveRestartEnabled(prim_restart);
    dynamic_state.SetRasterizerDiscardEnabled(regs.clipper_control.dx_rasterization_kill);
    dynamic_state.SetCullMode(cull_mode);
    dynamic_state.SetFrontFace(front_face);
}

void Rasterizer::UpdateRasterizationState() const {
    const auto& regs = Regs();
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetLineWidth(regs.line_control.Width());
}

void Rasterizer::UpdateColorBlendingState(const GraphicsPipeline* pipeline) const {
    const auto& regs = Regs();
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetBlendConstants(regs.blend_constants);
    dynamic_state.SetColorWriteMasks(pipeline->GetGraphicsKey().write_masks);
    dynamic_state.SetAttachmentFeedbackLoopEnabled(attachment_feedback_loop);
}

void Rasterizer::ScopeMarkerBegin(const std::string_view& str, bool from_guest) {
    DrainDrawPipe();
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
    });
}

void Rasterizer::ScopeMarkerEnd(bool from_guest) {
    DrainDrawPipe();
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.endDebugUtilsLabelEXT();
}

void Rasterizer::ScopedMarkerInsert(const std::string_view& str, bool from_guest) {
    DrainDrawPipe();
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
    });
}

void Rasterizer::ScopedMarkerInsertColor(const std::string_view& str, const u32 color,
                                         bool from_guest) {
    DrainDrawPipe();
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
        .color = std::array<f32, 4>(
            {(f32)((color >> 16) & 0xff) / 255.0f, (f32)((color >> 8) & 0xff) / 255.0f,
             (f32)(color & 0xff) / 255.0f, (f32)((color >> 24) & 0xff) / 255.0f})});
}

std::thread::id Rasterizer::GetGpuCommandProcessorThread() {
    return liverpool->GetGpuCommandProcessorThread();
}

#if defined(__linux__) || defined(_WIN32)
u32 Rasterizer::GetGpuCommandProcessorThreadId() {
    return liverpool->GetGpuCommandProcessorThreadId();
}
#endif

} // namespace Vulkan

namespace Vulkan {

void Rasterizer::MarkPass(const GraphicsPipeline* pipeline, const RenderState& state) {
    auto* profiler = GpuProfiler::Get();
    if (!profiler || !scheduler.WillBeginRendering(state)) {
        return;
    }
    const auto& vs = pipeline->GetStage(Shader::SwStage::Vertex);
    const auto* ps = pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
    std::array<u64, AmdGpu::NUM_COLOR_BUFFERS + 6> parts{};
    parts[0] = state.width | u64(state.height) << 32;
    parts[1] = vs.pgm_hash;
    parts[2] = ps ? ps->pgm_hash : 0;
    parts[3] = db_desc.first ? u64(texture_cache.GetImage(db_desc.first).info.pixel_format) : 0;
    for (u32 cb = 0; cb < state.num_color_attachments && cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
        parts[4 + cb] = cb_descs[cb].first
                            ? u64(texture_cache.GetImage(cb_descs[cb].first).info.pixel_format) + 1
                            : 0;
    }
    const u64 key = XXH3_64bits(parts.data(), sizeof(parts));
    profiler->Mark(key, [&] {
        std::string targets;
        for (u32 cb = 0; cb < state.num_color_attachments; ++cb) {
            targets += cb_descs[cb].first
                           ? vk::to_string(texture_cache.GetImage(cb_descs[cb].first).info.pixel_format)
                           : std::string{"-"};
            targets += ' ';
        }
        return fmt::format("pass {}x{} [{}] depth {} vs {:016x} ps {:016x}", state.width,
                           state.height, targets,
                           db_desc.first ? vk::to_string(
                                               texture_cache.GetImage(db_desc.first).info.pixel_format)
                                         : std::string{"-"},
                           vs.pgm_hash, ps ? ps->pgm_hash : 0);
    });
}

void Rasterizer::NoteFrameStart() {
    if (auto* profiler = GpuProfiler::Get()) {
        profiler->BeginFrame();
    }
    const u64 frame = Gow3Stats::gpu_frames.fetch_add(1, std::memory_order_relaxed) + 1;
    // GOW3_BUFFER_STATS=1: how many earlier frames the GPU has not finished when the GPU thread
    // starts a frame — the lag that decides whether guest memory can be read in place.
    static const bool stats = [] {
        const char* value = std::getenv("GOW3_BUFFER_STATS");
        return value && value[0] == '1';
    }();
    if (!stats) {
        return;
    }
    static std::array<u64, 16> frame_ticks{};
    static std::array<u64, 17> lag_histogram{};
    u32 lag = 0;
    for (u32 back = 1; back < frame_ticks.size() && back < frame; ++back) {
        const u64 tick = frame_ticks[(frame - back) % frame_ticks.size()];
        if (tick && !scheduler.IsFree(tick)) {
            lag = back;
        }
    }
    ++lag_histogram[lag];
    frame_ticks[frame % frame_ticks.size()] = scheduler.CurrentTick();
    if (frame % 600 == 0) {
        std::printf("GPU lag at frame start (frames not finished: count):");
        for (u32 i = 0; i < lag_histogram.size(); ++i) {
            if (lag_histogram[i]) {
                std::printf(" %u:%llu", i, static_cast<unsigned long long>(lag_histogram[i]));
            }
        }
        std::printf("\n");
        lag_histogram = {};
    }
}

} // namespace Vulkan
