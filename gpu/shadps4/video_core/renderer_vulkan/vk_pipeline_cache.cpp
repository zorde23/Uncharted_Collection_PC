// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <mutex>
#include <ranges>
#include <string>
#include <unordered_set>
#include <x86intrin.h>
#include "gow3_threads.h"

#include "common/hash.h"
#include "common/thread.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/ir/passes/srt.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#include "gow3_settings.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_depth_stencil_state.h"
#include "video_core/renderer_vulkan/motion_history.h"
#include "video_core/renderer_vulkan/vk_draw_prep.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

using Shader::HwStage;
using Shader::Output;
using Shader::SwStage;

struct AsyncShaderCompilation {
    AsyncJob<std::shared_ptr<AsyncShaderCompilation>> job;
    Shader::Info info;
    Shader::RuntimeInfo runtime{};
    std::vector<u32> code, user_data, geometry_code, binary;
    std::unique_ptr<Shader::Pools> pools;
    CompilerPool<Shader::Pools>* pool_cache{};
    std::optional<Shader::IR::Program> ir;
    Shader::StageSpecialization spec;
    Shader::Gcn::FetchShaderSnapshot fetch;
    Shader::Backend::Bindings binding;
    vk::Device device;
    vk::ShaderModule module{};
    size_t permutation{};
    bool initial{};
    ~AsyncShaderCompilation() {
        ir.reset();
        if (pools) {
            pool_cache->Recycle(std::move(pools));
        }
        if (module) {
            device.destroyShaderModule(module);
        }
    }
};

struct AsyncGraphicsCompilation {
    AsyncJob<std::shared_ptr<AsyncGraphicsCompilation>> job;
    PipelineSelection selection;
    std::array<std::unique_ptr<Shader::Info>, MaxShaderStages> infos;
    std::array<std::vector<u32>, MaxShaderStages> user_data;
    std::array<Shader::Gcn::FetchShaderSnapshot, MaxShaderStages> fetch;
    std::array<const Shader::Info*, MaxShaderStages> live_infos;
    GraphicsPipeline::SerializationSupport data;
    std::unique_ptr<GraphicsPipeline> pipeline;
    bool failure_logged = false;
};

constexpr static auto SpirvVersion1_6 = 0x00010600U;

constexpr static std::array DescriptorHeapSizes = {
    vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, 512},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, 1024},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampler, 1024},
};

static u32 MapOutputs(std::span<Shader::OutputMap, 3> outputs, const AmdGpu::VsOutputControl& ctl) {
    u32 num_outputs = 0;

    if (ctl.vs_out_misc_enable) {
        auto& misc_vec = outputs[num_outputs++];
        misc_vec[0] = ctl.use_vtx_point_size ? Output::PointSize : Output::None;
        misc_vec[1] = ctl.use_vtx_edge_flag
                          ? Output::EdgeFlag
                          : (ctl.use_vtx_gs_cut_flag ? Output::GsCutFlag : Output::None);
        misc_vec[2] =
            ctl.use_vtx_kill_flag
                ? Output::KillFlag
                : (ctl.use_vtx_render_target_idx ? Output::RenderTargetIndex : Output::None);
        misc_vec[3] = ctl.use_vtx_viewport_idx ? Output::ViewportIndex : Output::None;
    }

    if (ctl.vs_out_ccdist0_enable) {
        auto& ccdist0 = outputs[num_outputs++];
        ccdist0[0] = ctl.IsClipDistEnabled(0)
                         ? Output::ClipDist0
                         : (ctl.IsCullDistEnabled(0) ? Output::CullDist0 : Output::None);
        ccdist0[1] = ctl.IsClipDistEnabled(1)
                         ? Output::ClipDist1
                         : (ctl.IsCullDistEnabled(1) ? Output::CullDist1 : Output::None);
        ccdist0[2] = ctl.IsClipDistEnabled(2)
                         ? Output::ClipDist2
                         : (ctl.IsCullDistEnabled(2) ? Output::CullDist2 : Output::None);
        ccdist0[3] = ctl.IsClipDistEnabled(3)
                         ? Output::ClipDist3
                         : (ctl.IsCullDistEnabled(3) ? Output::CullDist3 : Output::None);
    }

    if (ctl.vs_out_ccdist1_enable) {
        auto& ccdist1 = outputs[num_outputs++];
        ccdist1[0] = ctl.IsClipDistEnabled(4)
                         ? Output::ClipDist4
                         : (ctl.IsCullDistEnabled(4) ? Output::CullDist4 : Output::None);
        ccdist1[1] = ctl.IsClipDistEnabled(5)
                         ? Output::ClipDist5
                         : (ctl.IsCullDistEnabled(5) ? Output::CullDist5 : Output::None);
        ccdist1[2] = ctl.IsClipDistEnabled(6)
                         ? Output::ClipDist6
                         : (ctl.IsCullDistEnabled(6) ? Output::CullDist6 : Output::None);
        ccdist1[3] = ctl.IsClipDistEnabled(7)
                         ? Output::ClipDist7
                         : (ctl.IsCullDistEnabled(7) ? Output::CullDist7 : Output::None);
    }

    return num_outputs;
}

const Shader::RuntimeInfo& PipelineCache::BuildRuntimeInfo(PipelineSelection& sel, HwStage stage,
                                                             SwStage l_stage) {
    auto& info = sel.runtime_infos[u32(l_stage)];
    const auto& regs = (*sel.regs);
    const auto BuildCommon = [&](const auto& program) {
        info.props.fp_denorm_mode32 = program.settings.fp_denorm_mode32;
        info.props.fp_denorm_mode16_64 = program.settings.fp_denorm_mode64;
        info.props.fp_round_mode32 = program.settings.fp_round_mode32;
        info.props.fp_round_mode16_64 = program.settings.fp_round_mode64;
        info.props.num_allocated_vgprs = program.NumVgprs();
        info.props.num_user_data = program.settings.num_user_regs;
        info.props.num_input_vgprs = program.settings.vgpr_comp_cnt;
        info.props.dx10_clamp = program.settings.dx10_clamp;
    };
    info.Initialize(stage, l_stage);
    switch (stage) {
    case HwStage::Local: {
        BuildCommon(regs.ls_program);
        Shader::TessellationDataConstantBuffer tess_constants{};
        const auto* hull_info = sel.infos[u32(SwStage::TessellationControl)];
        hull_info->ReadTessConstantBuffer(tess_constants);
        info.hw.ls.ls_stride = tess_constants.ls_stride;
        break;
    }
    case HwStage::Hull:
        BuildCommon(regs.hs_program);
        break;
    case HwStage::Export:
        BuildCommon(regs.es_program);
        info.hw.es.vertex_data_size = regs.vgt_esgs_ring_itemsize;
        break;
    case HwStage::Geometry: {
        BuildCommon(regs.gs_program);
        info.hw.gs.num_outputs = MapOutputs(info.hw.gs.outputs, regs.vs_output_control);
        info.hw.gs.output_vertices = regs.vgt_gs_max_vert_out;
        info.hw.gs.num_invocations =
            regs.vgt_gs_instance_cnt.IsEnabled() ? regs.vgt_gs_instance_cnt.count : 1;
        if (regs.stage_enable.raw == AmdGpu::ShaderStageEnable::LsHsEsGs) {
            info.hw.gs.in_primitive = [&]() {
                switch (regs.tess_config.topology) {
                case AmdGpu::TessellationTopology::Point:
                    return AmdGpu::PrimitiveType::PointList;
                case AmdGpu::TessellationTopology::Line:
                    return AmdGpu::PrimitiveType::LineList;
                case AmdGpu::TessellationTopology::TriangleCw:
                case AmdGpu::TessellationTopology::TriangleCcw:
                    return AmdGpu::PrimitiveType::TriangleList;
                default:
                    UNREACHABLE();
                }
            }();
        } else {
            info.hw.gs.in_primitive = regs.primitive_type;
        }
        for (u32 stream_id = 0; stream_id < Shader::GsMaxOutputStreams; ++stream_id) {
            info.hw.gs.out_primitive[stream_id] =
                regs.vgt_gs_out_prim_type.GetPrimitiveType(stream_id);
        }
        info.hw.gs.in_vertex_data_size = regs.vgt_esgs_ring_itemsize;
        info.hw.gs.out_vertex_data_size = regs.vgt_gs_vert_itemsize[0];
        info.hw.gs.mode = regs.vgt_gs_mode.mode;
        const auto params_vc = AmdGpu::GetParams(regs.vs_program);
        info.hw.gs.vs_copy = params_vc.code;
        info.hw.gs.vs_copy_hash = params_vc.hash;
        DumpShader(info.hw.gs.vs_copy, info.hw.gs.vs_copy_hash, Shader::HwStage::Vertex, 0,
                   "copy.bin");
        break;
    }
    case HwStage::Vertex: {
        BuildCommon(regs.vs_program);
        info.hw.vs.user_clip_plane_mask = regs.clipper_control.user_clip_plane_enable;
        info.hw.vs.num_outputs = MapOutputs(info.hw.vs.outputs, regs.vs_output_control);
        info.hw.vs.emulate_depth_negative_one_to_one =
            !instance.IsDepthClipControlSupported() &&
            regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW;
        info.hw.vs.clip_disable = regs.IsClipDisabled();
        info.hw.vs.motion_vectors = sel.motion;
        break;
    }
    case HwStage::Fragment: {
        BuildCommon(regs.ps_program);
        info.hw.fs.en_flags = regs.ps_input_ena;
        info.hw.fs.addr_flags = regs.ps_input_addr;
        info.hw.fs.num_inputs = regs.num_interp;
        info.hw.fs.front_face_all_bits = regs.barycentric_control.front_face_all_bits;
        info.hw.fs.depth_before_shader = regs.depth_shader_control.depth_before_shader;
        info.hw.fs.num_samples =
            regs.ps_input_addr.sample_coverage_ena && regs.ps_input_ena.sample_coverage_ena
                ? regs.aa_config.NumSamples()
                : 1;
        info.hw.fs.z_export_format = regs.z_export_format;
        info.hw.fs.motion_vectors = sel.motion;
        u8 stencil_ref_export_enable = regs.depth_shader_control.stencil_op_val_export_enable |
                                       regs.depth_shader_control.stencil_test_val_export_enable;
        info.hw.fs.mrtz_mask = regs.depth_shader_control.z_export_enable |
                               (stencil_ref_export_enable << 1) |
                               (regs.depth_shader_control.mask_export_enable << 2) |
                               (regs.depth_shader_control.coverage_to_mask_enable << 3);
        const auto& cb0_blend = regs.blend_control[0];
        if (cb0_blend.enable) {
            info.hw.fs.dual_source_blending =
                LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.color_dst_factor) ||
                LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.color_src_factor);
            if (cb0_blend.separate_alpha_blend) {
                info.hw.fs.dual_source_blending |=
                    LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.alpha_dst_factor) ||
                    LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.alpha_src_factor);
            }
        } else {
            info.hw.fs.dual_source_blending = false;
        }
        const auto& ps_inputs = regs.ps_inputs;
        for (u32 i = 0; i < regs.num_interp; i++) {
            info.hw.fs.inputs[i] = {
                .param_index = u16(ps_inputs[i].input_offset),
                .is_default = u16(ps_inputs[i].use_default),
                .is_flat = u16(ps_inputs[i].flat_shade),
                .default_value = u16(ps_inputs[i].default_value),
            };
        }
        for (u32 i = 0; i < Shader::MaxColorBuffers; i++) {
            info.hw.fs.color_buffers[i] = sel.graphics_key.color_buffers[i];
        }
        // Lowered user clip planes ride the same emulation path as guest-exported distances, so
        // the fragment side arms whenever the hardware vertex stage lowers them, keeping its input
        // locations in sync with the shifted vertex outputs.
        const bool lowers_user_clip_planes =
            regs.clipper_control.user_clip_plane_enable &&
            !regs.stage_enable.IsStageEnabled(static_cast<u32>(HwStage::Geometry));
        info.hw.fs.clip_distance_emulation =
            ((regs.vs_output_control.clip_distance_enable &&
              !regs.stage_enable.IsStageEnabled(static_cast<u32>(HwStage::Local))) ||
             lowers_user_clip_planes) &&
            profile.needs_clip_distance_emulation;
        break;
    }
    case HwStage::Compute: {
        const auto& cs_pgm = liverpool->GetCsRegs();
        info.props.num_user_data = cs_pgm.settings.num_user_regs;
        info.props.num_allocated_vgprs = cs_pgm.settings.num_vgprs * 4;
        info.props.fp_denorm_mode32 = cs_pgm.settings.fp_denorm_mode32;
        info.props.fp_denorm_mode16_64 = cs_pgm.settings.fp_denorm_mode64;
        info.props.fp_round_mode32 = cs_pgm.settings.fp_round_mode32;
        info.props.fp_round_mode16_64 = cs_pgm.settings.fp_round_mode64;
        info.props.dx10_clamp = cs_pgm.settings.dx10_clamp;
        info.hw.cs.workgroup_size = {cs_pgm.num_thread_x.full, cs_pgm.num_thread_y.full,
                                     cs_pgm.num_thread_z.full};
        info.hw.cs.tgid_enable = {cs_pgm.IsTgidEnabled(0), cs_pgm.IsTgidEnabled(1),
                                  cs_pgm.IsTgidEnabled(2)};
        info.hw.cs.shared_memory_size = cs_pgm.SharedMemSize();
        break;
    }
    default:
        break;
    }
    switch (l_stage) {
    case SwStage::Vertex:
        info.sw.vs.step_rate_0 = regs.vgt_instance_step_rate_0;
        info.sw.vs.step_rate_1 = regs.vgt_instance_step_rate_1;
        info.sw.vs.vertex_sgpr_offset = sel.draw_indirect_params.vertex_sgpr_offset;
        info.sw.vs.instance_sgpr_offset = sel.draw_indirect_params.instance_sgpr_offset;
        info.sw.vs.is_indirect = sel.draw_indirect_params.is_indirect;
        info.sw.vs.tess_emulated_primitive =
            regs.primitive_type == AmdGpu::PrimitiveType::RectList ||
            regs.primitive_type == AmdGpu::PrimitiveType::QuadList;
        break;
    case SwStage::TessellationControl: {
        info.sw.tcs.num_input_control_points = regs.ls_hs_config.hs_input_control_points;
        info.sw.tcs.num_threads = regs.ls_hs_config.hs_output_control_points;
        info.sw.tcs.tess_type = regs.tess_config.type;
        info.sw.tcs.offchip_lds_enable = regs.hs_program.settings.oc_lds_en;
        break;
    }
    case SwStage::TessellationEval: {
        info.sw.tes.tess_type = regs.tess_config.type;
        info.sw.tes.tess_topology = regs.tess_config.topology;
        info.sw.tes.tess_partitioning = regs.tess_config.partitioning;
        break;
    }
    default:
        break;
    }
    return info;
}

PipelineCache::PipelineCache(const Instance& instance_, Scheduler& scheduler_,
                             AmdGpu::Liverpool* liverpool_, u32 sparse_page_shift)
    : instance{instance_}, scheduler{scheduler_}, liverpool{liverpool_},
      desc_heap{instance, scheduler.GetWorkSemaphore(), DescriptorHeapSizes} {
    sel.regs = &liverpool->regs;
    const auto& vk12_props = instance.GetVk12Properties();
    profile = Shader::Profile{
        .max_viewport_width = instance.GetMaxViewportWidth(),
        .max_viewport_height = instance.GetMaxViewportHeight(),
        .max_shared_memory_size = instance.MaxComputeSharedMemorySize(),
        .supported_spirv = SpirvVersion1_6,
        .subgroup_size = instance.SubgroupSize(),
        .sparse_page_shift = sparse_page_shift,
        .support_int8 = instance.IsShaderInt8Supported(),
        .support_int16 = instance.IsShaderInt16Supported(),
        .support_int64 = instance.IsShaderInt64Supported(),
        .support_float16 = instance.IsShaderFloat16Supported(),
        .support_float64 = instance.IsShaderFloat64Supported(),
        .supports_denorm_behavior_independence =
            vk12_props.denormBehaviorIndependence != vk::ShaderFloatControlsIndependence::eNone,
        .supports_rounding_mode_independence =
            vk12_props.roundingModeIndependence != vk::ShaderFloatControlsIndependence::eNone,
        .support_fp16_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat16),
        .support_fp16_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat16),
        .support_fp16_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat16),
        .support_fp32_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat32),
        .support_fp32_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat32),
        .support_fp32_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat32),
        .support_fp64_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat64),
        .support_fp64_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat64),
        .support_fp64_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat64),
        .support_fp16_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat16),
        .support_fp32_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat32),
        .support_fp64_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat64),
        .supports_image_load_store_lod = instance_.IsImageLoadStoreLodSupported(),
        .supports_native_cube_calc = instance_.IsAmdGcnShaderSupported(),
        .supports_trinary_minmax = instance_.IsAmdShaderTrinaryMinMaxSupported(),
        .supports_buffer_fp32_atomic_min_max =
            instance_.IsShaderAtomicFloatBuffer32MinMaxSupported(),
        .supports_image_fp32_atomic_min_max = instance_.IsShaderAtomicFloatImage32MinMaxSupported(),
        .supports_buffer_int64_atomics = instance_.IsBufferInt64AtomicsSupported(),
        .supports_shared_int64_atomics = instance_.IsSharedInt64AtomicsSupported(),
        .supports_workgroup_explicit_memory_layout =
            instance_.IsWorkgroupMemoryExplicitLayoutSupported(),
        .supports_amd_shader_explicit_vertex_parameter =
            instance_.IsAmdShaderExplicitVertexParameterSupported(),
        .supports_fragment_shader_barycentric = instance_.IsFragmentShaderBarycentricSupported(),
        .supports_shader_subgroup_clock = instance_.IsShaderSubgroupClockSupported(),
        .needs_manual_interpolation = instance.IsFragmentShaderBarycentricSupported() &&
                                      instance.GetDriverID() == vk::DriverId::eNvidiaProprietary,
        // gow3: older NVIDIA (Pascal) has no barycentrics; GOW3_INTERP_INT_FIX=0/1 overrides.
        .needs_integer_interpolation_fix = [&] {
            if (const char* env = std::getenv("GOW3_INTERP_INT_FIX")) {
                return env[0] == '1';
            }
            return !instance.IsFragmentShaderBarycentricSupported() &&
                   instance.GetDriverID() == vk::DriverId::eNvidiaProprietary;
        }(),
        .needs_lds_barriers = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary ||
                              instance.GetDriverID() == vk::DriverId::eMesaKosmickrisp,
        .needs_buffer_offsets = instance.StorageMinAlignment() > 4,
        .needs_unorm_fixup = instance.GetDriverID() == vk::DriverId::eMesaKosmickrisp,
        .needs_clip_distance_emulation = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary,
        .supports_shader_stencil_export = instance_.IsShaderStencilExportSupported(),
    };

    auto [cache_result, cache] = instance.GetDevice().createPipelineCacheUnique({});
    ASSERT_MSG(cache_result == vk::Result::eSuccess, "Failed to create pipeline cache: {}",
               vk::to_string(cache_result));
    pipeline_cache = std::move(cache);
    // gow3: the compiler threads always exist; the in-game menu switches async compilation live
    // (Gow3Settings::async_shaders, GOW3_ASYNC_SHADERS at start).
    {
        const auto count = std::clamp(Gow3Threads::Available() / 2, 1u, 6u);
        const auto capture_count = std::min(2u, std::max(1u, Gow3Threads::Available() / 4));
        for (unsigned i = 0; i < count; ++i) {
            auto [result, worker_cache] = instance.GetDevice().createPipelineCacheUnique({});
            ASSERT_MSG(result == vk::Result::eSuccess, "Failed to create compiler pipeline cache");
            worker_pipeline_caches.push_back(std::move(worker_cache));
        }
        capture_compiler = std::make_unique<AsyncCompiler>(capture_count, 8, [](unsigned index) {
            Common::SetCurrentThreadName(("gow3:ShaderCapture" + std::to_string(index)).c_str());
            Common::SetCurrentThreadPriority(Common::ThreadPriority::High);
            Rasterizer::MarkCaptureThread(); // uc3: its page faults resolve inline
        });
        compiler = std::make_unique<AsyncCompiler>(count, 64, [](unsigned index) {
            Common::SetCurrentThreadName(("gow3:ShaderCompile" + std::to_string(index)).c_str());
            Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
        });
        LOG_INFO(Render_Vulkan, "gow3: async graphics compilation ready ({} capture, {} compile workers)",
                 capture_count, count);
    }
}

PipelineCache::~PipelineCache() {
    StopWarmUp();
    FinishCompilations();
}

// gow3: shader/pipeline compile time on the GPU thread, reported by GOW3_FRAME_STATS.
std::atomic<u64> g_gow3_compile_ns;
std::atomic<u32> g_gow3_compiles;
std::atomic<u64> g_gow3_capture_ns, g_gow3_async_ns, g_gow3_compile_wait_ns;
std::atomic<u64> g_gow3_snapshot_ns, g_gow3_guest_wait_ns;
// uc3: GOW3_STAGEB_PROFILE splits stage A's "A:gfx pipeline" step: [0] publish/pump cycles,
// [1] prepared-pipeline check cycles, [2] calls; inside the check: [3] GetParams, [4] SRT walk
// (RefreshFlatBuf), [5] flat compare cycles; prep trust (R.10 A1): [6] changed-since, [7] value
// compare, [8] trusted apply cycles, of them [9] program check + flat copy, [10] pipeline find.
std::atomic<u64> g_gow3_gfx_pipe_split[11];
static bool GfxPipeSplit() {
    static const bool on = std::getenv("GOW3_STAGEB_PROFILE") != nullptr;
    return on;
}
std::atomic<u32> g_gow3_pool_reuses;
// uc3: GOW3_A1_DIVERGENCE_LOG=1 - diagnostic: on a prep trust flat divergence, print why the draw
// was trusted (stamp state, value check) and the dwords that differ (trusted vs recomputed).
static bool A1DivergenceLog() {
    static const bool on = [] {
        const char* env = std::getenv("GOW3_A1_DIVERGENCE_LOG");
        return env && env[0] == '1';
    }();
    return on;
}
// uc3 R.10.17 (Fable): GOW3_A1_PENDING_CHECK=1 - a draw is trusted only when no GPU write is
// still pending (not landed in guest memory) over the guest ranges its walks read, whatever the
// stamps say: the stamp at emission says a write happened, the A's walk syncs it before reading.
static bool A1PendingCheck() {
    static const bool on = [] {
        const char* env = std::getenv("GOW3_A1_PENDING_CHECK");
        return env && env[0] == '1';
    }();
    return on;
}
// uc3 R.10.17 (Fable): by value only with clean pages: a page the walks read, written since the
// snapshot (even outside the ranges read), is a miss after a global stamp or a wait instead of a
// value compare. Part of A1 (on with GOW3_PREP_TRUST_A1=1); GOW3_A1_STRICT_VALUE=0 turns it off
// for a test.
static bool A1StrictValue() {
    static const bool on = [] {
        const char* env = std::getenv("GOW3_A1_STRICT_VALUE");
        return env ? std::atoi(env) != 0 : SrtA1();
    }();
    return on;
}
// uc3 R.10.17: wall clock for the divergence lines (local time, ms).
static void A1Clock(char (&text)[16]) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    const std::tm* tm = std::localtime(&t);
    std::snprintf(text, sizeof(text), "%02d:%02d:%02d.%03d", tm ? tm->tm_hour : 0,
                  tm ? tm->tm_min : 0, tm ? tm->tm_sec : 0, int(ms));
}
struct A1TrustContext {
    u32 stamp_why = ~0u; ///< why from the stamps, before the value check
    u32 origin = ~0u;
    bool by_value = false;
    bool values_hold = false;
    bool after_wait = false;
    bool label_unmet = false;
    u32 num_checks = 0;
    // R.10.17 diagnostics, at the trust decision: guest runs with a GPU write still pending.
    u32 pending_runs = 0;
    u64 pending_addr = 0;
    u32 pending_size = 0;
    char pending_what[96] = {};
};
static A1TrustContext g_a1_ctx;
std::atomic<u32> g_gow3_pending_draws;
namespace {
struct PhaseTimer {
    std::atomic<u64>& counter;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    bool active = true;
    void Stop() {
        if (!active) {
            return;
        }
        counter += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now() - start).count());
        active = false;
    }
    ~PhaseTimer() { Stop(); }
};
struct CompileTimer {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~CompileTimer() {
        g_gow3_compile_ns += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now() - start)
                                   .count());
        ++g_gow3_compiles;
    }
};
} // namespace

bool PipelineCache::QueueShader(Program& program, HwStage stage, SwStage l_stage,
                                 const Shader::ShaderParams& params,
                                 Shader::RuntimeInfo runtime_info,
                                 Shader::Backend::Bindings binding, size_t permutation,
                                 bool initial,
                                 std::optional<Shader::StageSpecialization> specialization) {
    if (!compiler->HasCapacity()) {
        return false;
    }
    PhaseTimer snapshot{g_gow3_snapshot_ns};
    auto build = std::make_shared<AsyncShaderCompilation>();
    build->device = instance.GetDevice();
    build->code.assign(params.code.begin(), params.code.end());
    build->user_data.assign(params.user_data.begin(), params.user_data.end());
    build->info = Shader::Info(stage, l_stage, params);
    build->info.user_data = build->user_data;
    build->runtime = runtime_info;
    if (stage == HwStage::Geometry && !runtime_info.hw.gs.vs_copy.empty()) {
        build->geometry_code.assign(runtime_info.hw.gs.vs_copy.begin(), runtime_info.hw.gs.vs_copy.end());
        build->runtime.hw.gs.vs_copy = build->geometry_code;
    }
    build->binding = binding;
    build->permutation = permutation;
    build->initial = initial;
    if (specialization) {
        build->spec = *specialization;
    }
    build->pool_cache = &compiler_pools;
    snapshot.Stop();
    // uc3: the capture thread may read a page the GPU wrote (precise readbacks) and bring the
    // GPU's data back itself, submitting as the draw recording thread does. That thread runs
    // dry before the capture starts: two threads finishing at once deadlock on the submission
    // (one holds its mutex waiting for the recording to sync, the other waits for the mutex
    // with recorded commands the sync needs).
    if (auto* rasterizer = liverpool->GetRasterizer()) {
        rasterizer->DrainDrawPipe();
    }
    AsyncJob<bool> capture_job;
    if (!capture_job.Start(*capture_compiler, [this, build, binding] {
        PhaseTimer capture{g_gow3_capture_ns};
        auto [pools, reused] = compiler_pools.Acquire();
        build->pools = std::move(pools);
        if (reused) {
            ++g_gow3_pool_reuses;
        }
        // gow3: the GPU thread waits until all guest reads and specialization are captured.
        build->ir.emplace(Shader::TranslateProgram(build->code, *build->pools, build->info,
                                                   build->runtime, profile));
        build->spec = Shader::StageSpecialization(build->info, build->runtime, profile, binding);
        build->fetch.Capture(build->info);
        return true;
    })) {
        return false;
    }
    {
        PhaseTimer waiting{g_gow3_guest_wait_ns};
        capture_job.Wait();
    }
    if (!capture_job.Poll()) {
        if (initial) {
            program.compilation_failed = true;
        } else {
            specialization->info = &program.info;
            program.failed_permutations.push_back(std::move(*specialization));
        }
        LOG_ERROR(Render_Vulkan, "gow3: shader {:#x}/{} capture failed: {}", params.hash,
                  permutation, capture_job.Error());
        return false;
    }
    const bool queued = build->job.Start(*compiler, [this, build] {
        PhaseTimer compile{g_gow3_async_ns};
        Shader::Gcn::ScopedFetchShaderSnapshot snapshot{build->fetch};
        auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, build->runtime, *build->ir,
                                                    build->binding);
        DumpShader(build->code, build->info.pgm_hash, build->info.hw_stage, build->permutation, "bin");
        DumpShader(spv, build->info.pgm_hash, build->info.hw_stage, build->permutation, "spv");
        auto patch = GetShaderPatch(build->info.pgm_hash, build->info.hw_stage,
                                    build->permutation, "spv");
        build->module = CompileSPV(patch && EmulatorSettings.IsPatchShaders() ? *patch : spv,
                                    instance.GetDevice());
        build->binary = std::move(spv);
        build->ir.reset();
        compiler_pools.Recycle(std::move(build->pools));
        return build;
    }, true);
    if (queued) {
        program.pending = std::move(build);
        pending_programs.push_back(&program);
    }
    return queued;
}

void PipelineCache::PublishShaders(bool wait) {
    for (size_t i = 0; i < pending_programs.size();) {
        auto* program = pending_programs[i];
        const auto hash = program->info.pgm_hash;
        const auto retire = [&] {
            pending_programs[i] = pending_programs.back();
            pending_programs.pop_back();
        };
        auto build = program->pending;
        if (wait) {
            build->job.Wait();
        }
        auto result = build->job.Poll();
        if (!result) {
            if (build->job.Failed()) {
                if (build->initial) {
                    program->compilation_failed = true;
                } else {
                    build->spec.info = &program->info;
                    program->failed_permutations.push_back(std::move(build->spec));
                }
                program->pending.reset();
                LOG_ERROR(Render_Vulkan, "gow3: shader {:#x}/{} compilation failed: {}", hash,
                          build->permutation, build->job.Error());
                retire();
            } else {
                ++i;
            }
            continue;
        }
        const auto perm_hash = HashCombine(hash, build->permutation);
        Vulkan::SetObjectName(instance.GetDevice(), build->module,
                             GetShaderName(build->info.hw_stage, hash, build->permutation));
        RegisterShaderBinary(std::move(build->binary), hash, build->permutation);
        {
            std::unique_lock lock{programs_mutex};
            if (build->initial) {
                program->info = std::move(build->info);
                // user_data is refreshed on the next lookup; no pointer into a completed job survives.
                program->info.user_data = {};
                program->info_template = std::make_unique<Shader::Info>(program->info);
                program->info_template->trusted_flat = nullptr; // R.10 A1 v8
            }
            build->spec.info = &program->info;
            RegisterShaderMeta(program->info, Shader::Gcn::ToOptional(build->spec.fetch_shader_data), build->spec,
                               perm_hash, build->permutation);
            program->AddPermut(build->module, std::move(build->spec));
            build->module = nullptr;
            program->pending.reset();
        }
        retire();
    }
}

void PipelineCache::PublishGraphics(bool wait) {
    for (auto it = pending_graphics.begin(); it != pending_graphics.end();) {
        auto build = it.value();
        if (wait) {
            build->job.Wait();
        }
        auto result = build->job.Poll();
        if (!result) {
            if (build->job.Failed() && !build->failure_logged) {
                LOG_ERROR(Render_Vulkan, "gow3: pipeline {:#x} compilation failed: {}",
                          std::hash<GraphicsPipelineKey>{}(it.key()), build->job.Error());
                build->failure_logged = true;
            }
            ++it;
            continue;
        }
        const auto key = it.key();
        build->pipeline->PublishStages(build->live_infos);
        RegisterPipelineData(key, std::hash<GraphicsPipelineKey>{}(key), build->data);
        graphics_pipelines.emplace(key, std::move(build->pipeline));
        ++num_new_pipelines;
        it = pending_graphics.erase(it);
    }
}

void PipelineCache::FinishCompilations() {
    if (compiler) {
        capture_compiler->Stop();
        compiler->Stop();
        PublishShaders(true);
        PublishGraphics(true);
        std::vector<vk::PipelineCache> sources;
        for (const auto& cache : worker_pipeline_caches) {
            sources.push_back(*cache);
        }
        const auto result = instance.GetDevice().mergePipelineCaches(*pipeline_cache, sources);
        if (result != vk::Result::eSuccess) {
            LOG_ERROR(Render_Vulkan, "gow3: compiler cache merge failed: {}", vk::to_string(result));
        }
    }
}

// uc3: GOW3_SYNC_SMALL_TARGETS=1: draws into small color targets (at most 64K pixels) compile
// their shaders before drawing even with background compilation on. Background compilation
// skips a draw until its shaders are ready, and some small targets are drawn only once: the
// 256x8 light falloff table drawn at boot stayed black and the cave torch lit nothing.
static bool SyncSmallTarget(const AmdGpu::Regs& regs) {
    static const bool on = [] {
        const char* env = std::getenv("GOW3_SYNC_SMALL_TARGETS");
        return env && env[0] == '1';
    }();
    if (!on) {
        return false;
    }
    bool any = false;
    for (u32 i = 0; i < AmdGpu::NUM_COLOR_BUFFERS; ++i) {
        const auto& col_buf = regs.color_buffers[i];
        if (!col_buf || !regs.color_target_mask.GetMask(i)) {
            continue;
        }
        if (u64(col_buf.Pitch()) * col_buf.Height() > 65536) {
            return false;
        }
        any = true;
    }
    return any;
}

bool PipelineCache::AsyncCompilation() const {
    // uc3: GOW3_FORCE_SYNC_SHADERS=1 compiles every shader before its draw (diagnostic).
    static const bool force_sync = std::getenv("GOW3_FORCE_SYNC_SHADERS") != nullptr;
    if (force_sync) {
        return false;
    }
    return compiler && Gow3Settings::Get().async_shaders.load(std::memory_order_relaxed);
}

void PipelineCache::SeedWorkerCaches() {
    if (!compiler || worker_caches_seeded) {
        return;
    }
    for (const auto& cache : worker_pipeline_caches) {
        const auto result = instance.GetDevice().mergePipelineCaches(*cache, *pipeline_cache);
        ASSERT_MSG(result == vk::Result::eSuccess, "Failed to seed compiler pipeline cache");
    }
    worker_caches_seeded = true;
}

void PipelineCache::ReportCompilerStats() {
    if (compiler) {
        std::printf("Compiler stats: capture queue %zu, compile queue %zu; queue waits: capture "
                    "%.1f ms, compile %.1f ms; pools retained %zu, reused %u\n",
                    capture_compiler->Depth(), compiler->Depth(),
                    capture_compiler->QueueWaitNs() / 1e6, compiler->QueueWaitNs() / 1e6,
                    compiler_pools.Retained(), g_gow3_pool_reuses.exchange(0));
    }
}

bool PipelineCache::PrepareGraphicsPipeline(PipelineSelection& worker_sel) {
    // Tessellation stages read constant buffers from memory at selection time: not prepared.
    if (worker_sel.regs->stage_enable.hs_en) {
        return false;
    }
    return RefreshGraphicsKey(worker_sel) && !worker_sel.worker->failed;
}

void PipelineCache::NotePrepTrustDivergence(const char* what, u64 hash, VAddr base) {
    if (++prep_trust_divergences <= 20) {
        char clock[16];
        A1Clock(clock);
        std::printf("uc3 R.22 prep trust divergence: %s, shader %#llx at %#llx (%s)\n", what,
                    static_cast<unsigned long long>(hash), static_cast<unsigned long long>(base),
                    clock);
    }
}

void PipelineCache::ReportPrepTrust() {
    if (prep_trust_hits + prep_trust_misses == 0) {
        return;
    }
    u64 bumps[4];
    for (u32 i = 0; i < 4; ++i) {
        bumps[i] = g_srt_bumps[i].exchange(0, std::memory_order_relaxed);
    }
    using ull = unsigned long long;
    std::printf("uc3 R.22 prep trust: %llu trusted (%llu by value), %llu not (GPU leaves %llu, values changed %llu, "
                "big write %llu, page write %llu, too many pages %llu, after a wait %llu, too many waits %llu), %llu divergences (total); "
                "stamps: waits %llu, page writes %llu, big writes %llu, unnoted packets %llu; "
                "A1 waits not met %llu (GPU-GPU %llu); after a CPU wait trusted by value %llu, values changed %llu; wait label page written %llu (wait no longer met %llu); value compares %llu (%.1f runs, %.0f bytes each, %llu deduped from the previous draw); "
                "waits before draws: CPU %llu, semaphore %llu, rewind %llu; "
                "trusted by value after global/big/page write %llu/%llu/%llu, "
                "values changed after a CPU wait/global/big/page write %llu/%llu/%llu/%llu; "
                "trusted pipeline reused from the previous draw %llu (why 2/3 never go by value)\n",
                (ull)prep_trust_hits, (ull)prep_trust_validated, (ull)prep_trust_misses, (ull)prep_trust_why[0],
                (ull)prep_trust_why[1], (ull)prep_trust_why[2], (ull)prep_trust_why[3],
                (ull)prep_trust_why[4], (ull)prep_trust_why[5], (ull)prep_trust_why[6],
                (ull)prep_trust_divergences, (ull)bumps[0],
                (ull)bumps[1], (ull)bumps[2], (ull)bumps[3],
                (ull)g_srt_waits_unmet.exchange(0, std::memory_order_relaxed),
                (ull)g_srt_waits_gpu.exchange(0, std::memory_order_relaxed),
                (ull)prep_trust_wait_validated, (ull)prep_trust_wait_changed,
                (ull)prep_trust_label_page, (ull)prep_trust_label_unmet, (ull)prep_trust_check_calls,
                prep_trust_check_calls ? double(prep_trust_checks) / prep_trust_check_calls : 0.0,
                prep_trust_check_calls ? double(prep_trust_check_bytes) / prep_trust_check_calls : 0.0,
                (ull)prep_trust_check_skips,
                (ull)g_srt_wait_kinds[0].exchange(0, std::memory_order_relaxed),
                (ull)g_srt_wait_kinds[1].exchange(0, std::memory_order_relaxed),
                (ull)g_srt_wait_kinds[2].exchange(0, std::memory_order_relaxed),
                (ull)prep_trust_stamp_validated[1], (ull)prep_trust_stamp_validated[2],
                (ull)prep_trust_stamp_validated[3], (ull)prep_trust_stamp_changed[0],
                (ull)prep_trust_stamp_changed[1], (ull)prep_trust_stamp_changed[2],
                (ull)prep_trust_stamp_changed[3], (ull)prep_trust_pipe_reused);
    prep_trust_pipe_reused = 0;
    {
        static constexpr const char* names[] = {"other",   "packet",  "guest write", "pending gpu",
                                                "landing", "binding", "rewrite", "readback"};
        std::printf("uc3 R.10 page write misses (why 3) %llu, by stamp origin (misses: flat "
                    "same/different): ",
                    (ull)prep_trust_why[3]);
        for (u32 i = 0; i < SrtNumOrigins; ++i) {
            std::printf("%s %llu: %llu/%llu (ranges missed/hit/unknown %llu/%llu/%llu); ",
                        names[i], (ull)prep_trust_pw_origin[i], (ull)prep_trust_pw_needless[i],
                        (ull)prep_trust_pw_real[i], (ull)prep_trust_pw_missed[i],
                        (ull)prep_trust_pw_hit[i], (ull)prep_trust_pw_unknown[i]);
            prep_trust_pw_origin[i] = prep_trust_pw_needless[i] = prep_trust_pw_real[i] = 0;
            prep_trust_pw_missed[i] = prep_trust_pw_hit[i] = prep_trust_pw_unknown[i] = 0;
        }
        std::printf("stamped page held a table pointer %llu, leaves only %llu\n",
                    (ull)prep_trust_pw_ptr, (ull)prep_trust_pw_leaf);
        prep_trust_pw_ptr = prep_trust_pw_leaf = 0;
    }
    for (u32 i = 0; i < 4; ++i) {
        prep_trust_stamp_validated[i] = prep_trust_stamp_changed[i] = 0;
    }
    {
        std::scoped_lock lk{g_srt_wait_sites_mutex};
        if (g_srt_num_wait_sites != 0) {
            auto& sites = g_srt_wait_sites;
            std::sort(sites.begin(), sites.begin() + g_srt_num_wait_sites,
                      [](const SrtWaitSite& a, const SrtWaitSite& b) { return a.count > b.count; });
            std::printf("  A1 CPU wait sites (label, function, last ref: count; %u sites, %llu lost):",
                        g_srt_num_wait_sites, (ull)g_srt_wait_sites_lost);
            for (u32 i = 0; i < std::min<u32>(g_srt_num_wait_sites, 8); ++i) {
                std::printf(" %#llx f%u %#x: %llu;", (ull)sites[i].address, sites[i].function,
                            sites[i].ref, (ull)sites[i].count);
            }
            std::printf("\n");
            g_srt_num_wait_sites = 0;
            g_srt_wait_sites_lost = 0;
        }
    }
    if (A1PendingCheck() || A1StrictValue()) {
        std::printf("uc3 R.10.17 A1: pending GPU write misses %llu (%llu runs tested, %.0f cycles "
                    "each), strict value misses %llu\n",
                    (ull)prep_trust_pending, (ull)prep_trust_pending_runs,
                    prep_trust_pending_runs ? double(prep_trust_pending_cycles) /
                                                  double(prep_trust_pending_runs)
                                            : 0.0,
                    (ull)prep_trust_strict);
        prep_trust_pending = prep_trust_pending_runs = prep_trust_pending_cycles = 0;
        prep_trust_strict = 0;
    }
    prep_trust_wait_validated = prep_trust_wait_changed = prep_trust_label_page = 0;
    prep_trust_label_unmet = prep_trust_check_calls = prep_trust_checks = 0;
    prep_trust_check_bytes = prep_trust_check_skips = 0;
    prep_trust_hits = prep_trust_misses = prep_trust_validated = 0;
    for (auto& why : prep_trust_why) {
        why = 0;
    }
}

const GraphicsPipeline* PipelineCache::TryPreparedPipeline(const PreparedDraw& prepared) {
    // R.10 A1: only the immediately preceding call's draw can lend its value compare.
    const PreparedDraw* prev_value_ok = last_value_ok;
    last_value_ok = nullptr;
    if (prepared.state.load(std::memory_order_acquire) != PreparedDraw::Ready ||
        prepared.reg_checksum != liverpool->gfx_reg_checksum) {
        return nullptr;
    }
    // Same registers; the stage programs and their flattened user data (which also covers
    // everything read from guest memory for the specialization) must match as well. This is
    // the per-stage work GetProgram does on the regular path.
    const auto& regs = liverpool->regs;
    // uc3 R.20/R.21 (GOW3_PREP_TRUST): with the same registers, no GPU-written dwords in the
    // worker's walk and no write to the pages it read (nor a wait, nor a big write) since its
    // snapshot, the worker's flattened user data stands: no shader footer search and no SRT
    // walk here. =2 is the shadow mode: the regular check runs and is compared with what trust
    // would have used. Unset, GOW3_PREP_TRUST_A1=1 alone turns trust on (the launcher option).
    static const int trust_mode = [] {
        const char* env = std::getenv("GOW3_PREP_TRUST");
        return env ? std::atoi(env) : (SrtA1() ? 1 : 0);
    }();
    bool trusted = false;
    static_assert(SrtNumOrigins <= 8);
    u32 pw_origin = ~0u; ///< R.10 A1 v8: origin of the page write that missed this draw
    if (trust_mode != 0) {
        u32 why;
        if (!prepared.no_gpu_leaves) {
            why = 0;
        } else if (prepared.after_wait && prepared.wait_kind != 1) {
            why = 5;
        } else if (prepared.num_read_pages > prepared.read_pages.size()) {
            why = 4;
        } else {
            const bool split = GfxPipeSplit();
            const u64 c0 = split ? __rdtsc() : 0;
            u32 range_hits = 2; ///< R.10.17: 0 when pages read were written outside the ranges
            // R.10.17 (Fable): a GPU write still pending over the runs read is a miss for every
            // draw about to be trusted (stamps or values), tested before the stamps: a landing
            // stamps its pages before it stops being pending, so one that lands after this test
            // is seen by the stamps below.
            const bool pending_log = A1DivergenceLog();
            A1TrustContext pending_ctx{};
            bool pending_hit = false;
            if ((A1PendingCheck() || pending_log) && prepared.num_guest_runs != 0) {
                auto* const rasterizer = liverpool->GetRasterizer();
                const u64 p0 = __rdtsc();
                u32 tested = 0;
                for (u32 r = 0; rasterizer && r < prepared.num_guest_runs; ++r) {
                    const auto& run = prepared.guest_runs[r];
                    ++tested;
                    if (!rasterizer->IsGpuWritten(run.guest, run.size)) {
                        continue;
                    }
                    pending_hit = true;
                    if (pending_ctx.pending_runs++ == 0 && pending_log) {
                        pending_ctx.pending_addr = run.guest;
                        pending_ctx.pending_size = run.size;
                        const char* what = rasterizer->DescribeGpuWritten(run.guest, run.size);
                        std::snprintf(pending_ctx.pending_what, sizeof(pending_ctx.pending_what),
                                      "%s", what ? what : "");
                    }
                    if (!pending_log) {
                        break;
                    }
                }
                prep_trust_pending_runs += tested;
                prep_trust_pending_cycles += __rdtsc() - p0;
            }
            why = SrtChangedSince(prepared.srt_snap, prepared.read_pages.data(),
                                  prepared.num_read_pages);
            const u64 c1 = split ? __rdtsc() : 0;
            if (why == 3) {
                const u32 index = SrtChangedPage(prepared.srt_snap, prepared.read_pages.data(),
                                                 prepared.num_read_pages);
                u32 origin = SrtOriginOther;
                if (index < prepared.num_read_pages) {
                    origin = g_srt_page_origin[prepared.read_pages[index] % SrtPageSlots].load(
                        std::memory_order_relaxed);
                    origin = origin < SrtNumOrigins ? origin : SrtOriginOther;
                    ++((prepared.ptr_page_mask >> index) & 1 ? prep_trust_pw_ptr
                                                             : prep_trust_pw_leaf);
                }
                // R.10 A1 v9 (Fable): a page write that missed every range the walks read
                // leaves the flat as it was. Such a draw is trusted as why 0 (by value only
                // after a wait, as any why 0).
                if (SrtA1()) {
                    u32 page_hits = 2;
                    why = SrtChangedSinceRanges(prepared.srt_snap, prepared.read_pages.data(),
                                                prepared.num_read_pages, prepared.guest_runs,
                                                prepared.num_guest_runs, page_hits);
                    range_hits = page_hits;
                    u64* const resolved = page_hits == 0   ? prep_trust_pw_missed
                                          : page_hits == 1 ? prep_trust_pw_hit
                                                           : prep_trust_pw_unknown;
                    ++resolved[origin];
                }
                if (why == 3) {
                    pw_origin = origin;
                    ++prep_trust_pw_origin[origin];
                }
            }
            if (A1StrictValue() && range_hits == 0 && (why == 1 || prepared.after_wait)) {
                ++prep_trust_strict;
                why = 3;
            }
            if (A1PendingCheck() && pending_hit && why < 2) {
                ++prep_trust_pending;
                why = 3;
            }
            // R.10 A1 (Fable): the label pages are not in the read set: labels share pages
            // densely, so a write to one of them only matters when a met wait no longer holds.
            // Only the waits whose label page was written are re-tested.
            bool label_unmet = false;
            if (why < 2) {
                for (u32 i = 0; i < prepared.num_wait_pages; ++i) {
                    if (SrtChangedSince(prepared.srt_snap, &prepared.wait_pages[i], 1) < 2) {
                        continue;
                    }
                    ++prep_trust_label_page;
                    if (!SrtMetWaitsHold(prepared, prepared.wait_pages[i])) {
                        ++prep_trust_label_unmet;
                        label_unmet = true;
                        why = 3;
                        break;
                    }
                }
            }
            // uc3 R.22: after a wait or an unnoted write, the values the walk read decide: equal
            // values give the same flat (GPU writes still pending are on the page stamps).
            // =3 trusts without the value check (the shadow runs never saw a value change after
            // a wait; measurement only).
            // R.10 A1 (Fable): after a CPU wait (met by now) the CPU writes it released are
            // visible but not stamped: the values the walk read decide as well.
            // R.10 A1: a big or page write since the snapshot stays a miss. Deciding it by the
            // values (v7) diverged in the shadow run: the A's walk syncs GPU writes still pending
            // (SrtReadDword / fixup) while the compare reads memory before they land.
            const bool wait_check = prepared.after_wait;
            const u32 origin = why == 0 ? 0 : why;
            const bool by_value = !label_unmet && (why == 1 || (why == 0 && wait_check));
            bool values_hold = false;
            if (by_value) {
                // R.10 A1 (Fable): runs equal in the previous draw, which held in the same wait
                // epoch, are not compared again; only when no stamp moved since the snapshot
                // (a stamped write between the two draws may be in a deduped run).
                const bool skip_same = why == 0 && prepared.dedup_prev != nullptr &&
                                       prepared.dedup_prev == prev_value_ok;
                u32 bytes = 0;
                values_hold = trust_mode == 3 ||
                              Shader::SrtChecksHold(prepared.checks, prepared.num_checks,
                                                    prepared.check_bytes, skip_same, &bytes);
                ++prep_trust_check_calls;
                prep_trust_checks += prepared.num_checks;
                prep_trust_check_bytes += bytes;
                prep_trust_check_skips += skip_same;
                if (values_hold && trust_mode != 3) {
                    last_value_ok = &prepared;
                }
            }
            if (A1DivergenceLog()) {
                g_a1_ctx = {why, origin, by_value, values_hold, wait_check, label_unmet,
                            prepared.num_checks};
                g_a1_ctx.pending_runs = pending_ctx.pending_runs;
                g_a1_ctx.pending_addr = pending_ctx.pending_addr;
                g_a1_ctx.pending_size = pending_ctx.pending_size;
                std::memcpy(g_a1_ctx.pending_what, pending_ctx.pending_what,
                            sizeof(g_a1_ctx.pending_what));
            }
            if (by_value && values_hold) {
                why = 0;
                ++prep_trust_validated;
                ++prep_trust_stamp_validated[origin];
                if (wait_check) {
                    ++prep_trust_wait_validated;
                }
            } else if (by_value) {
                ++prep_trust_stamp_changed[origin];
                if (why == 0) {
                    why = 1;
                }
                if (wait_check) {
                    ++prep_trust_wait_changed;
                }
            }
            if (split) {
                g_gow3_gfx_pipe_split[6].fetch_add(c1 - c0, std::memory_order_relaxed);
                g_gow3_gfx_pipe_split[7].fetch_add(__rdtsc() - c1, std::memory_order_relaxed);
            }
        }
        trusted = prepared.no_gpu_leaves && why == 0;
        if (trusted) {
            ++prep_trust_hits;
        } else {
            ++prep_trust_misses;
            ++prep_trust_why[why];
        }
    }
    if (trusted && (trust_mode == 1 || trust_mode == 3)) {
        const bool split = GfxPipeSplit();
        const u64 c0 = split ? __rdtsc() : 0;
        for (u32 i = 0; i < prepared.num_stages; ++i) {
            const auto& stage = prepared.stages[i];
            const auto* pgm = regs.ProgramForStage(static_cast<u32>(stage.hw_stage));
            if (!pgm || pgm->Address<VAddr>() != stage.pgm_base) {
                return nullptr;
            }
        }
        for (u32 i = 0; i < prepared.num_stages; ++i) {
            const auto& stage = prepared.stages[i];
            const auto* pgm = regs.ProgramForStage(static_cast<u32>(stage.hw_stage));
            auto& info = const_cast<Program*>(stage.program)->info;
            info.pgm_base = stage.pgm_base;
            info.user_data = std::span<const u32, Shader::ShaderParams::NumShaderUserData>{pgm->user_data};
            // R.10 A1 v8 (Fable 2): the flat stays in the arena; the packet points at it.
            info.trusted_flat = stage.flat;
            info.trusted_flat_size = stage.flat_size;
            info.gpu_leaves.clear();
        }
        const u64 c1 = split ? __rdtsc() : 0;
        // R.10 A1 v8 (Fable 1a/1b): consecutive draws mostly share the key; the previous
        // trusted pipeline is reused when the key is the same (its own copy, compared here),
        // otherwise the worker's hash saves hashing the key again.
        const GraphicsPipeline* pipeline = nullptr;
        if (last_trusted_pipe && prepared.key_hash == last_trusted_hash &&
            prepared.key == last_trusted_key) {
            pipeline = last_trusted_pipe;
            ++prep_trust_pipe_reused;
        } else {
            const auto it = graphics_pipelines.find(prepared.key, prepared.key_hash);
            if (it != graphics_pipelines.end() && it->second) {
                pipeline = it->second.get();
                last_trusted_pipe = pipeline;
                last_trusted_hash = prepared.key_hash;
                last_trusted_key = prepared.key;
            }
        }
        if (!pipeline) {
            // The regular path refreshes the flats; until then nothing reads the arena ones.
            for (u32 i = 0; i < prepared.num_stages; ++i) {
                const_cast<Program*>(prepared.stages[i].program)->info.trusted_flat = nullptr;
            }
        }
        if (split) {
            const u64 c2 = __rdtsc();
            g_gow3_gfx_pipe_split[8].fetch_add(c2 - c0, std::memory_order_relaxed);
            g_gow3_gfx_pipe_split[9].fetch_add(c1 - c0, std::memory_order_relaxed);
            g_gow3_gfx_pipe_split[10].fetch_add(c2 - c1, std::memory_order_relaxed);
        }
        return pipeline;
    }
    for (u32 i = 0; i < prepared.num_stages; ++i) {
        const auto& stage = prepared.stages[i];
        const auto* pgm = regs.ProgramForStage(static_cast<u32>(stage.hw_stage));
        if (!pgm || !pgm->Address<u32*>()) {
            return nullptr;
        }
        const bool split = GfxPipeSplit();
        const u64 c0 = split ? __rdtsc() : 0;
        const auto params = AmdGpu::GetParams(*pgm);
        const u64 c1 = split ? __rdtsc() : 0;
        if (trusted && (params.hash != stage.hash || params.Base() != stage.pgm_base)) {
            NotePrepTrustDivergence("program", stage.hash, params.Base());
        }
        if (params.hash != stage.hash) {
            return nullptr;
        }
        auto& info = const_cast<Program*>(stage.program)->info;
        info.pgm_base = params.Base();
        info.user_data = params.user_data;
        info.RefreshFlatBuf();
        const u64 c2 = split ? __rdtsc() : 0;
        const bool flat_differs =
            info.pgm_base != stage.pgm_base || info.flattened_ud_buf.size() != stage.flat_size ||
            std::memcmp(info.flattened_ud_buf.data(), stage.flat, stage.flat_size * sizeof(u32)) != 0;
        if (split) {
            g_gow3_gfx_pipe_split[3].fetch_add(c1 - c0, std::memory_order_relaxed);
            g_gow3_gfx_pipe_split[4].fetch_add(c2 - c1, std::memory_order_relaxed);
            g_gow3_gfx_pipe_split[5].fetch_add(__rdtsc() - c2, std::memory_order_relaxed);
        }
        if (flat_differs) {
            if (trusted) {
                NotePrepTrustDivergence("flat", stage.hash, info.pgm_base);
                if (A1DivergenceLog()) {
                    const auto& ctx = g_a1_ctx;
                    char clock[16];
                    A1Clock(clock);
                    std::printf("uc3 A1 divergence detail (%s): shader %#llx stage %u base %#llx->%#llx "
                                "flat dwords %u->%zu; stamps why %u origin %u pw_origin %d, by value %d "
                                "(values held %d, %u checks), after a wait %d, label unmet %d; "
                                "pending GPU write at trust: %u of %u runs (first %#llx+%u: %s); runs:",
                                clock, (unsigned long long)stage.hash, i,
                                (unsigned long long)stage.pgm_base,
                                (unsigned long long)info.pgm_base, u32(stage.flat_size),
                                info.flattened_ud_buf.size(), ctx.stamp_why, ctx.origin,
                                pw_origin == ~0u ? -1 : int(pw_origin), int(ctx.by_value),
                                int(ctx.values_hold), ctx.num_checks, int(ctx.after_wait),
                                int(ctx.label_unmet), ctx.pending_runs, prepared.num_guest_runs,
                                (unsigned long long)ctx.pending_addr, ctx.pending_size,
                                ctx.pending_what);
                    for (u32 r = 0; r < prepared.num_guest_runs && r < 8; ++r) {
                        const auto& run = prepared.guest_runs[r];
                        const u64 page = run.guest >> 12;
                        std::printf(" %#llx+%u(o%u)", (unsigned long long)run.guest, run.size,
                                    u32(g_srt_page_origin[page % SrtPageSlots].load(
                                        std::memory_order_relaxed)));
                    }
                    std::printf("; differ:");
                    const size_t n = std::min<size_t>(stage.flat_size, info.flattened_ud_buf.size());
                    u32 shown = 0;
                    u32 total = 0;
                    for (size_t d = 0; d < n; ++d) {
                        if (stage.flat[d] != info.flattened_ud_buf[d]) {
                            ++total;
                            if (shown < 24) {
                                ++shown;
                                std::printf(" [%zu] %#x->%#x", d, stage.flat[d],
                                            info.flattened_ud_buf[d]);
                            }
                        }
                    }
                    std::printf(" (%u of %zu dwords; user data %zu dwords)\n", total, n,
                                info.user_data.size());
                }
            }
            if (pw_origin < SrtNumOrigins) {
                ++prep_trust_pw_real[pw_origin];
            }
            return nullptr;
        }
        if (trusted && !info.gpu_leaves.empty()) {
            NotePrepTrustDivergence("gpu leaves", stage.hash, info.pgm_base);
        }
    }
    if (pw_origin < SrtNumOrigins) {
        ++prep_trust_pw_needless[pw_origin];
    }
    const auto it = graphics_pipelines.find(prepared.key);
    return it != graphics_pipelines.end() ? it->second.get() : nullptr;
}

const GraphicsPipeline* PipelineCache::GetGraphicsPipeline(const DrawIndirectParams params,
                                                           const PreparedDraw* prepared) {
    const bool split = GfxPipeSplit();
    const u64 t0 = split ? __rdtsc() : 0;
    SeedWorkerCaches();
    PublishShaders();
    PublishGraphics();
    PumpWarmUp();
    used_prepared = nullptr;
    const u64 t1 = split ? __rdtsc() : 0;
    if (split) {
        g_gow3_gfx_pipe_split[0].fetch_add(t1 - t0, std::memory_order_relaxed);
        g_gow3_gfx_pipe_split[2].fetch_add(1, std::memory_order_relaxed);
    }
    if (prepared) {
        const auto* pipeline = TryPreparedPipeline(*prepared);
        if (split) {
            g_gow3_gfx_pipe_split[1].fetch_add(__rdtsc() - t1, std::memory_order_relaxed);
        }
        if (pipeline) {
            used_prepared = prepared;
            return pipeline;
        }
    }
    sel.draw_indirect_params = params;
    if (!RefreshGraphicsKey(sel)) {
        if (sel.compilation_pending) {
            ++g_gow3_pending_draws;
        }
        return nullptr;
    }
    if (AsyncCompilation() && !sel.regs->stage_enable.hs_en && !EmulatorSettings.IsShaderCollect() &&
        !SyncSmallTarget(*sel.regs)) {
        const auto found = graphics_pipelines.find(sel.graphics_key);
        if (found != graphics_pipelines.end()) {
            return found->second.get();
        }
        if (!pending_graphics.contains(sel.graphics_key) && compiler->HasCapacity()) {
            PhaseTimer capture{g_gow3_snapshot_ns};
            auto build = std::make_shared<AsyncGraphicsCompilation>();
            build->selection = sel;
            build->selection.regs = nullptr;
            build->live_infos = sel.infos;
            for (u32 i = 0; i < MaxShaderStages; ++i) {
                if (sel.infos[i]) {
                    build->infos[i] = std::make_unique<Shader::Info>(*sel.infos[i]);
                    build->user_data[i].assign(sel.infos[i]->user_data.begin(), sel.infos[i]->user_data.end());
                    build->infos[i]->user_data = build->user_data[i];
                    build->selection.infos[i] = build->infos[i].get();
                    build->fetch[i].Capture(*build->infos[i]);
                }
            }
            const bool queued = build->job.Start(*compiler, [this, build] {
                // Only the vertex stage uses fetch-shader guest pointers during pipeline construction.
                Shader::Gcn::ScopedFetchShaderSnapshot snapshot{build->fetch[u32(SwStage::Vertex)]};
                PhaseTimer compile{g_gow3_async_ns};
                auto& s = build->selection;
                build->pipeline = std::make_unique<GraphicsPipeline>(
                    instance, scheduler, desc_heap, profile, s.graphics_key,
                    *worker_pipeline_caches[AsyncCompiler::WorkerIndex()],
                    s.infos, s.runtime_infos, s.fetch_shader, s.modules, build->data, false);
                return build;
            });
            if (queued) {
                pending_graphics.emplace(sel.graphics_key, std::move(build));
            }
        }
        ++g_gow3_pending_draws;
        return nullptr;
    }
    const auto [it, is_new] = graphics_pipelines.try_emplace(sel.graphics_key);
    if (is_new) {
        const auto pipeline_hash = std::hash<GraphicsPipelineKey>{}(sel.graphics_key);
        LOG_INFO(Render_Vulkan, "Compiling graphics pipeline {:#x}", pipeline_hash);
        CompileTimer timer;

        GraphicsPipeline::SerializationSupport sdata{};
        it.value() = std::make_unique<GraphicsPipeline>(
            instance, scheduler, desc_heap, profile, sel.graphics_key, *pipeline_cache, sel.infos,
            sel.runtime_infos, sel.fetch_shader, sel.modules, sdata, false);

        RegisterPipelineData(sel.graphics_key, pipeline_hash, sdata);
        ++num_new_pipelines;

        if (EmulatorSettings.IsShaderCollect()) {
            for (auto stage = 0; stage < MaxShaderStages; ++stage) {
                if (sel.infos[stage]) {
                    auto& m = sel.modules[stage];
                    module_related_pipelines[m].emplace_back(sel.graphics_key);
                }
            }
        }
        sel.fetch_shader.reset();
    }
    return it->second.get();
}

const ComputePipeline* PipelineCache::GetComputePipeline() {
    PublishShaders();
    PublishGraphics();
    PumpWarmUp();
    if (!RefreshComputeKey()) {
        return nullptr;
    }
    const auto [it, is_new] = compute_pipelines.try_emplace(compute_key);
    if (is_new) {
        const auto pipeline_hash = std::hash<ComputePipelineKey>{}(compute_key);
        LOG_INFO(Render_Vulkan, "Compiling compute pipeline {:#x}", pipeline_hash);
        CompileTimer timer;

        ComputePipeline::SerializationSupport sdata{};
        it.value() = std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile,
                                                       *pipeline_cache, compute_key, *sel.infos[0],
                                                       sel.modules[0], sdata, false);
        RegisterPipelineData(compute_key, sdata);
        ++num_new_pipelines;

        if (EmulatorSettings.IsShaderCollect()) {
            auto& m = sel.modules[0];
            module_related_pipelines[m].emplace_back(compute_key);
        }
    }
    return it->second.get();
}

bool PipelineCache::RefreshGraphicsKey(PipelineSelection& sel) {
    std::memset(&sel.graphics_key, 0, sizeof(GraphicsPipelineKey));
    const auto& regs = (*sel.regs);
    auto& key = sel.graphics_key;

    // gow3: a depth/stencil buffer that cannot affect the draw is not attached: on the PC a
    // smaller one would crop a full-screen pass (eltutz, vk_depth_stencil_state.h).
    const bool db_enabled = GetEffectiveDepthStencilState(regs).needs_attachment;

    key.z_format = db_enabled && regs.depth_buffer.DepthValid()
                       ? regs.depth_buffer.z_info.format
                       : AmdGpu::DepthBuffer::ZFormat::Invalid;
    key.stencil_format = db_enabled && regs.depth_buffer.StencilValid()
                             ? regs.depth_buffer.stencil_info.format
                             : AmdGpu::DepthBuffer::StencilFormat::Invalid;
    key.depth_clamp_enable = !regs.depth_render_override.disable_viewport_clamp;
    key.depth_clip_enable = regs.clipper_control.ZclipEnable();
    key.clip_space = regs.clipper_control.clip_space;
    key.provoking_vtx_last = regs.polygon_control.provoking_vtx_last;
    key.prim_type = regs.primitive_type;
    key.polygon_mode = regs.polygon_control.PolyMode();
    key.patch_control_points =
        regs.stage_enable.hs_en ? regs.ls_hs_config.hs_input_control_points : 0;
    key.logic_op = regs.color_control.rop3;
    key.depth_samples = db_enabled ? regs.depth_buffer.NumSamples() : 1;
    key.num_samples = key.depth_samples;
    key.cb_shader_mask = regs.color_shader_mask;

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;

    // Only potentially animated G-buffer draws may use the extra motion attachment. The
    // ordinary stage variant is needed first to inspect the vertex shader's resources.
    bool motion_possible = false;
    {
        u32 bound = 0;
        for (s32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS && !skip_cb_binding; ++cb) {
            bound += (regs.color_buffers[cb] && regs.color_target_mask.GetMask(cb)) ? 1 : 0;
        }
        motion_possible = Shader::MotionVectors::positions_address != 0 && bound >= 5 &&
                          !regs.color_buffers[Shader::MotionVectors::Output] &&
                          regs.depth_buffer.DepthValid() &&
                          regs.depth_buffer.NumSamples() == 1 && !regs.IsClipDisabled() &&
                          regs.stage_enable.raw == AmdGpu::ShaderStageEnable::VgtStages::Vs;
    }
    sel.motion = false;

    // First pass to fill render target information needed by shader recompiler
    for (s32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS && !skip_cb_binding; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        if (!col_buf || !regs.color_target_mask.GetMask(cb)) {
            // No attachment bound or writing to it is disabled.
            continue;
        }

        // Fill color target information
        auto& color_buffer = key.color_buffers[cb];
        color_buffer.data_format = col_buf.GetDataFmt();
        color_buffer.num_format = col_buf.GetNumberFmt();
        color_buffer.num_conversion = col_buf.GetNumberConversion();
        color_buffer.export_format = regs.color_export_format.GetFormat(cb);
        color_buffer.swizzle = col_buf.Swizzle();

    }

    // Compile and bind shader stages
    if (!RefreshGraphicsStages(sel)) {
        return false;
    }
    if (motion_possible) {
        const auto* vs = sel.infos[static_cast<u32>(Shader::SwStage::Vertex)];
        if (vs) {
            // Shaders with a bone palette (motion_history.h). Small skeletons (weapons,
            // props) also include static world pieces: the rasterizer gives those
            // history only while their constants change.
            for (const auto& resource : vs->buffers) {
                if (resource.IsSpecial()) continue;
                const auto buffer = resource.GetSharp(*vs);
                if (buffer.Valid() && buffer.GetStride() == 16 &&
                    Motion::ClassifyBuffer(buffer.GetSize()) != Motion::BufferRole::Other) {
                    sel.motion = true;
                    break;
                }
            }
        }
        // GOW3_MOTION_SELECT_LOG=1: each G-buffer vertex shader once, with its buffer sizes
        // and the selection, to find animated models that the size rule leaves out.
        static const bool select_log = [] {
            const char* value = std::getenv("GOW3_MOTION_SELECT_LOG");
            return value && value[0] == '1';
        }();
        if (select_log && vs) {
            std::string sizes;
            for (const auto& resource : vs->buffers) {
                if (resource.IsSpecial()) continue;
                const auto buffer = resource.GetSharp(*vs);
                sizes += fmt::format(" {}/{}", buffer.GetSize(), buffer.GetStride());
            }
            static std::mutex log_mutex;
            static std::unordered_set<u64> logged;
            std::scoped_lock lock{log_mutex};
            if (logged.insert(vs->pgm_hash ^ std::hash<std::string>{}(sizes)).second) {
                std::printf("Motion select: vs %016llx %s, buffers (size/stride):%s\n",
                            static_cast<unsigned long long>(vs->pgm_hash),
                            sel.motion ? "ON " : "off", sizes.c_str());
            }
        }
        // Keep the old broad path available for visual A/B tests.
        static const bool all_motion = [] {
            const char* value = std::getenv("GOW3_OBJECT_MOTION_ALL");
            return value && value[0] == '1';
        }();
        if (all_motion) {
            sel.motion = true;
        }
        if (sel.motion && !RefreshGraphicsStages(sel)) {
            return false;
        }
    }

    // Second pass to mask out render targets not written by shader and fill remaining info
    u8 color_samples = 0;
    bool all_color_samples_same = true;
    for (s32 cb = 0; cb < key.num_color_attachments && !skip_cb_binding; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
        if (!col_buf || !target_mask) {
            continue;
        }
        if ((key.mrt_mask & (1u << cb)) == 0) {
            std::memset(&key.color_buffers[cb], 0, sizeof(Shader::PsColorBuffer));
            continue;
        }

        // Fill color blending information
        if (regs.blend_control[cb].enable && !col_buf.info.blend_bypass) {
            key.blend_controls[cb] = regs.blend_control[cb];
        }

        // Apply swizzle to target mask
        key.write_masks[cb] =
            vk::ColorComponentFlags{key.color_buffers[cb].swizzle.ApplyMask(target_mask)};

        // Fill color samples
        const u8 prev_color_samples = std::exchange(color_samples, col_buf.NumSamples());
        all_color_samples_same &= color_samples == prev_color_samples || prev_color_samples == 0;
        key.color_samples[cb] = color_samples;
        key.num_samples = std::max(key.num_samples, color_samples);
    }

    if (sel.motion) {
        constexpr u32 mv = Shader::MotionVectors::Output;
        key.motion_vectors = 1;
        key.mrt_mask |= 1u << mv;
        key.num_color_attachments = mv + 1;
        auto& color_buffer = key.color_buffers[mv];
        color_buffer.data_format = AmdGpu::DataFormat::Format32_32_32_32;
        color_buffer.num_format = AmdGpu::NumberFormat::Float;
        color_buffer.swizzle = AmdGpu::IdentityMapping;
        key.write_masks[mv] = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                              vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        key.color_samples[mv] = 1;
    }

    // Force all color samples to match depth samples to avoid unsupported MSAA configuration
    if (color_samples != 0) {
        const bool depth_mismatch = db_enabled && color_samples != key.depth_samples;
        if (!all_color_samples_same && !instance.IsMixedAnySamplesSupported() ||
            all_color_samples_same && depth_mismatch && !instance.IsMixedDepthSamplesSupported()) {
            key.color_samples.fill(key.depth_samples);
            key.num_samples = key.depth_samples;
        }
    }

    return true;
}

bool PipelineCache::RefreshGraphicsStages(PipelineSelection& sel) {
    const auto& regs = (*sel.regs);
    auto& key = sel.graphics_key;
    sel.fetch_shader = std::nullopt;
    sel.compilation_pending = false;

    Shader::Backend::Bindings binding{};
    const auto bind_stage = [&](HwStage stage_in, SwStage stage_out) -> bool {
        const auto stage_in_idx = static_cast<u32>(stage_in);
        const auto stage_out_idx = static_cast<u32>(stage_out);
        if (!regs.stage_enable.IsStageEnabled(stage_in_idx)) {
            key.stage_hashes[stage_out_idx] = 0;
            sel.infos[stage_out_idx] = nullptr;
            return false;
        }

        const auto* pgm = regs.ProgramForStage(stage_in_idx);
        if (!pgm || !pgm->Address<u32*>()) {
            key.stage_hashes[stage_out_idx] = 0;
            sel.infos[stage_out_idx] = nullptr;
            return false;
        }

        const auto params = AmdGpu::GetParams(*pgm);
        std::optional<Shader::Gcn::FetchShaderData> fetch_shader_;
        std::tie(sel.infos[stage_out_idx], sel.modules[stage_out_idx], fetch_shader_,
                 key.stage_hashes[stage_out_idx]) =
            GetProgram(sel, stage_in, stage_out, params, binding);
        if (!sel.infos[stage_out_idx]) {
            sel.compilation_pending = true;
            // uc3: GOW3_SKIP_LOG=1: why a draw has no shader (cave torch table never drawn?).
            static const bool skip_log = std::getenv("GOW3_SKIP_LOG") != nullptr;
            if (skip_log && !sel.worker) {
                static std::atomic<int> shown{0};
                if (shown.fetch_add(1) < 300) {
                    std::shared_lock lk{programs_mutex};
                    const auto found = program_cache.find(params.hash);
                    const bool known = found != program_cache.end();
                    std::printf("ucndc: draw skipped: stage %u shader %#llx, known %d, failed %d, "
                                "pending %d, modules %zu, async %d\n",
                                stage_out_idx, (unsigned long long)params.hash, int(known),
                                known ? int(found->second->compilation_failed) : -1,
                                known ? int(found->second->pending != nullptr) : -1,
                                known ? found->second->modules.size() : size_t(0),
                                int(AsyncCompilation()));
                    std::fflush(stdout);
                }
            }
            return false;
        }
        if (fetch_shader_) {
            sel.fetch_shader = fetch_shader_;
        }
        return true;
    };

    sel.infos.fill(nullptr);
    sel.modules.fill(nullptr);

    bind_stage(HwStage::Fragment, SwStage::Fragment);
    if (sel.compilation_pending) {
        return false;
    }

    const auto* fs_info = sel.infos[static_cast<u32>(SwStage::Fragment)];
    key.mrt_mask = fs_info ? fs_info->mrt_mask : 0u;
    key.num_color_attachments = std::bit_width(key.mrt_mask);

    switch (regs.stage_enable.raw) {
    case AmdGpu::ShaderStageEnable::VgtStages::EsGs:
        if (!instance.IsGeometryStageSupported()) {
            LOG_WARNING(Render_Vulkan, "Geometry shader stage unsupported, skipping");
            return false;
        }
        if (regs.vgt_gs_mode.onchip || regs.vgt_strmout_config.raw) {
            LOG_WARNING(Render_Vulkan, "Geometry shader features unsupported, skipping");
            return false;
        }
        if (!bind_stage(HwStage::Export, SwStage::Vertex)) {
            return false;
        }
        if (!bind_stage(HwStage::Geometry, SwStage::Geometry)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHs:
        if (!instance.IsTessellationSupported()) {
            return false;
        }
        if (!bind_stage(HwStage::Hull, SwStage::TessellationControl)) {
            return false;
        }
        if (!bind_stage(HwStage::Vertex, SwStage::TessellationEval)) {
            return false;
        }
        if (!bind_stage(HwStage::Local, SwStage::Vertex)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHsEsGs:
        if (!instance.IsTessellationSupported()) {
            return false;
        }
        if (!instance.IsGeometryStageSupported()) {
            LOG_WARNING(Render_Vulkan, "Geometry shader stage unsupported, skipping");
            return false;
        }
        if (regs.vgt_gs_mode.onchip || regs.vgt_strmout_config.raw) {
            LOG_WARNING(Render_Vulkan, "Geometry shader features unsupported, skipping");
            return false;
        }
        if (!bind_stage(HwStage::Hull, SwStage::TessellationControl)) {
            return false;
        }
        if (!bind_stage(HwStage::Export, SwStage::TessellationEval)) {
            return false;
        }
        if (!bind_stage(HwStage::Local, SwStage::Vertex)) {
            return false;
        }
        if (!bind_stage(HwStage::Geometry, SwStage::Geometry)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::Vs:
        bind_stage(HwStage::Vertex, SwStage::Vertex);
        if (sel.compilation_pending) {
            return false;
        }
        break;
    default:
        LOG_WARNING(Render_Vulkan, "unimplemented shader stage {}", (u32)regs.stage_enable.raw);
        return false;
    }

    const auto* vs_info = sel.infos[static_cast<u32>(SwStage::Vertex)];
    if (vs_info && sel.fetch_shader && !instance.IsVertexInputDynamicState()) {
        // Without vertex input dynamic state, the pipeline needs to specialize on format.
        // Stride will still be handled outside the pipeline using dynamic state.
        u32 vertex_binding = 0;
        for (const auto& attrib : sel.fetch_shader->attributes) {
            const auto& buffer = attrib.GetSharp(*vs_info);
            ASSERT_MSG(vertex_binding < MaxVertexBufferCount,
                       "Vertex attribute binding count exceeded limit: {} >= {}", vertex_binding,
                       MaxVertexBufferCount);
            key.vertex_buffer_formats[vertex_binding++] =
                Vulkan::LiverpoolToVK::SurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt());
        }
    }

    return true;
}

bool PipelineCache::RefreshComputeKey() {
    Shader::Backend::Bindings binding{};
    const auto& cs_pgm = liverpool->GetCsRegs();
    const auto cs_params = AmdGpu::GetParams(cs_pgm);
    std::tie(sel.infos[0], sel.modules[0], sel.fetch_shader, compute_key.value) =
        GetProgram(sel, HwStage::Compute, SwStage::Compute, cs_params, binding);
    return sel.infos[0] != nullptr;
}

vk::ShaderModule PipelineCache::CompileModule(Shader::Info& info, Shader::RuntimeInfo& runtime_info,
                                              const std::span<const u32>& code, size_t perm_idx,
                                              Shader::Backend::Bindings& binding) {
    LOG_INFO(Render_Vulkan, "Compiling {} shader {:#x} {}", info.hw_stage, info.pgm_hash,
             perm_idx != 0 ? "(permutation)" : "");
    DumpShader(code, info.pgm_hash, info.hw_stage, perm_idx, "bin");
    CompileTimer timer;

    const auto ir_program = Shader::TranslateProgram(code, pools, info, runtime_info, profile);
    auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, ir_program, binding);
    DumpShader(spv, info.pgm_hash, info.hw_stage, perm_idx, "spv");

    vk::ShaderModule module;

    auto patch = GetShaderPatch(info.pgm_hash, info.hw_stage, perm_idx, "spv");
    const bool is_patched = patch && EmulatorSettings.IsPatchShaders();
    if (is_patched) {
        LOG_INFO(Loader, "Loaded patch for {} shader {:#x}", info.hw_stage, info.pgm_hash);
        module = CompileSPV(*patch, instance.GetDevice());
    } else {
        module = CompileSPV(spv, instance.GetDevice());
    }

    RegisterShaderBinary(std::move(spv), info.pgm_hash, perm_idx);

    const auto name = GetShaderName(info.hw_stage, info.pgm_hash, perm_idx);
    Vulkan::SetObjectName(instance.GetDevice(), module, name);
    if (EmulatorSettings.IsShaderCollect()) {
        DebugState.CollectShader(name, info.sw_stage, module, spv, code,
                                 patch ? *patch : std::span<const u32>{}, is_patched);
    }
    return module;
}

PipelineCache::Result PipelineCache::GetProgram(PipelineSelection& sel, HwStage hw_stage,
                                                SwStage sw_stage,
                                                const Shader::ShaderParams& params,
                                                Shader::Backend::Bindings& binding) {
    auto runtime_info = BuildRuntimeInfo(sel, hw_stage, sw_stage);
    if (sel.worker) {
        // gow3: draw-preparation worker: look up only, with the worker's own Info copy.
        auto& worker = *sel.worker;
        std::shared_lock lk{programs_mutex};
        const auto found_program = program_cache.find(params.hash);
        if (found_program == program_cache.end() || !found_program->second->info_template) {
            worker.failed = true;
            return {};
        }
        const Program* program = found_program->second.get();
        auto [it_info, new_info] = worker.infos.try_emplace(program, *program->info_template);
        auto& info = it_info->second;
        info.pgm_base = params.Base();
        info.user_data = params.user_data;
        // The walk of resource tables and the fetch shader parse read guest memory through
        // pointers in the registers; ahead of the GPU thread that memory may already be
        // reused. A fault returns here (runtime_fault_recover) and the draw is left to the GPU
        // thread. A jump out of the specialization leaks its partial allocations (rare).
        sigjmp_buf recover;
        if (sigsetjmp(recover, 0)) {
            Shader::t_srt_read_pages = nullptr;
            Shader::t_srt_ptr_pages = nullptr;
            Shader::t_srt_guest_runs = nullptr;
            Shader::t_srt_checks = nullptr;
            Shader::t_srt_check_bytes = nullptr;
            worker.failed = true;
            return {};
        }
        runtime_fault_recover = &recover;
        Shader::t_srt_read_pages = &worker.read_pages;
        Shader::t_srt_ptr_pages = &worker.ptr_pages;
        Shader::t_srt_guest_runs = &worker.guest_runs;
        Shader::t_srt_checks = &worker.checks;
        Shader::t_srt_check_bytes = &worker.check_bytes;
        info.RefreshFlatBuf();
        Shader::t_srt_read_pages = nullptr;
        Shader::t_srt_ptr_pages = nullptr;
        Shader::t_srt_guest_runs = nullptr;
        Shader::t_srt_checks = nullptr;
        Shader::t_srt_check_bytes = nullptr;
        auto spec = Shader::StageSpecialization(info, runtime_info, profile, binding);
        runtime_fault_recover = nullptr;
        const auto it = std::ranges::find(program->modules, spec, &Program::Module::spec);
        if (it == program->modules.end()) {
            worker.failed = true;
            return {};
        }
        info.AddBindings(binding);
        const size_t perm_idx = std::distance(program->modules.begin(), it);
        worker.stages.push_back(
            {program, params.hash, hw_stage, info.pgm_base, &info.flattened_ud_buf});
        return std::make_tuple(&info, it->module, Shader::Gcn::ToOptional(it->spec.fetch_shader_data),
                               HashCombine(params.hash, perm_idx));
    }

    auto it_pgm = program_cache.find(params.hash); // this thread is the only writer
    const bool compile_async = AsyncCompilation() && hw_stage != HwStage::Compute &&
                               !sel.regs->stage_enable.hs_en &&
                               !EmulatorSettings.IsShaderCollect() && !SyncSmallTarget(*sel.regs);
    if (!compile_async && it_pgm != program_cache.end() && it_pgm->second->pending) {
        {
            PhaseTimer waiting{g_gow3_compile_wait_ns};
            it_pgm->second->pending->job.Wait();
        }
        PublishShaders();
    }
    if (!compile_async && it_pgm != program_cache.end() && it_pgm->second->modules.empty() &&
        !it_pgm->second->compilation_failed) {
        std::unique_lock lock{programs_mutex};
        program_cache.erase(it_pgm);
        it_pgm = program_cache.end();
    }
    if (it_pgm == program_cache.end()) {
        auto new_program = std::make_unique<Program>(hw_stage, sw_stage, params);
        if (compile_async) {
            QueueShader(*new_program, hw_stage, sw_stage, params, runtime_info, binding, 0, true);
            std::unique_lock lock{programs_mutex};
            program_cache.emplace(params.hash, std::move(new_program));
            return {};
        }
        auto start = binding;
        const auto module =
            CompileModule(new_program->info, runtime_info, params.code, 0, binding);
        auto spec = Shader::StageSpecialization(new_program->info, runtime_info, profile, start);
        const auto perm_hash = HashCombine(params.hash, 0);

        RegisterShaderMeta(new_program->info, Shader::Gcn::ToOptional(spec.fetch_shader_data), spec,
                           perm_hash, 0);
        new_program->AddPermut(module, std::move(spec));
        new_program->info_template = std::make_unique<Shader::Info>(new_program->info);
        Program* program = new_program.get();
        {
            std::unique_lock lk{programs_mutex};
            program_cache.emplace(params.hash, std::move(new_program));
        }
        return std::make_tuple(&program->info, module,
                               Shader::Gcn::ToOptional(program->modules[0].spec.fetch_shader_data),
                               perm_hash);
    }

    auto& program = it_pgm.value();
    if (program->compilation_failed || (program->modules.empty() && program->pending)) {
        return {};
    }
    if (program->modules.empty() && compile_async) {
        QueueShader(*program, hw_stage, sw_stage, params, runtime_info, binding, 0, true);
        return {};
    }
    if (!program->info_template) {
        // Programs loaded by the pipeline cache warm-up get their template on first use.
        std::unique_lock lk{programs_mutex};
        program->info_template = std::make_unique<Shader::Info>(program->info);
        program->info_template->trusted_flat = nullptr; // R.10 A1 v8: arena flats stay on A
    }
    auto& info = program->info;
    info.pgm_base = params.Base(); // Needs to be actualized for inline cbuffer address fixup
    info.user_data = params.user_data;
    info.RefreshFlatBuf();
    auto spec = Shader::StageSpecialization(info, runtime_info, profile, binding);

    size_t perm_idx = program->modules.size();
    u64 perm_hash = HashCombine(params.hash, perm_idx);

    vk::ShaderModule module{};

    // gow3: consecutive draws of a program almost always use the same permutation.
    auto it = program->last_used < program->modules.size() &&
                      program->modules[program->last_used].spec == spec
                  ? program->modules.begin() + program->last_used
                  : std::ranges::find(program->modules, spec, &Program::Module::spec);
    if (it != program->modules.end()) {
        program->last_used = std::distance(program->modules.begin(), it);
    }
    if (it == program->modules.end()) {
        if (program->pending || std::ranges::find(program->failed_permutations, spec) !=
                                    program->failed_permutations.end()) {
            return {};
        }
        if (compile_async) {
            QueueShader(*program, hw_stage, sw_stage, params, runtime_info, binding, perm_idx, false,
                        std::move(spec));
            return {};
        }
        auto new_info = Shader::Info(hw_stage, sw_stage, params);
        module = CompileModule(new_info, runtime_info, params.code, perm_idx, binding);

        RegisterShaderMeta(info, Shader::Gcn::ToOptional(spec.fetch_shader_data), spec, perm_hash,
                           perm_idx);
        std::unique_lock lk{programs_mutex};
        program->AddPermut(module, std::move(spec));
    } else {
        info.AddBindings(binding);
        module = it->module;
        perm_idx = std::distance(program->modules.begin(), it);
        perm_hash = HashCombine(params.hash, perm_idx);
    }
    return std::make_tuple(&program->info, module,
                           Shader::Gcn::ToOptional(program->modules[perm_idx].spec.fetch_shader_data),
                           perm_hash);
}

std::optional<vk::ShaderModule> PipelineCache::ReplaceShader(vk::ShaderModule module,
                                                             std::span<const u32> spv_code) {
    PublishShaders(true);
    PublishGraphics(true);
    std::optional<vk::ShaderModule> new_module{};
    for (const auto& [_, program] : program_cache) {
        for (auto& m : program->modules) {
            if (m.module == module) {
                const auto& d = instance.GetDevice();
                d.destroyShaderModule(m.module);
                m.module = CompileSPV(spv_code, d);
                new_module = m.module;
            }
        }
    }
    if (module_related_pipelines.contains(module)) {
        auto& pipeline_keys = module_related_pipelines[module];
        for (auto& key : pipeline_keys) {
            if (std::holds_alternative<GraphicsPipelineKey>(key)) {
                auto& graphics_key = std::get<GraphicsPipelineKey>(key);
                graphics_pipelines.erase(graphics_key);
                last_trusted_pipe = nullptr;
            } else if (std::holds_alternative<ComputePipelineKey>(key)) {
                auto& compute_key = std::get<ComputePipelineKey>(key);
                compute_pipelines.erase(compute_key);
            }
        }
    }
    return new_module;
}

std::string PipelineCache::GetShaderName(Shader::HwStage stage, u64 hash,
                                         std::optional<size_t> perm) {
    if (perm) {
        return fmt::format("{}_{:#018x}_{}", stage, hash, *perm);
    }
    return fmt::format("{}_{:#018x}", stage, hash);
}

void PipelineCache::DumpShader(std::span<const u32> code, u64 hash, Shader::HwStage stage,
                               size_t perm_idx, std::string_view ext) {
    if (!EmulatorSettings.IsDumpShaders()) {
        return;
    }

    using namespace Common::FS;
    const auto dump_dir = GetUserPath(PathType::ShaderDir) / "dumps";
    if (!std::filesystem::exists(dump_dir)) {
        std::filesystem::create_directories(dump_dir);
    }
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    const auto file = IOFile{dump_dir / filename, FileAccessMode::Create};
    file.WriteSpan(code);
}

std::optional<std::vector<u32>> PipelineCache::GetShaderPatch(u64 hash, Shader::HwStage stage,
                                                              size_t perm_idx,
                                                              std::string_view ext) {

    using namespace Common::FS;
    const auto patch_dir = GetUserPath(PathType::ShaderDir) / "patch";
    if (!std::filesystem::exists(patch_dir)) {
        std::filesystem::create_directories(patch_dir);
    }
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    const auto filepath = patch_dir / filename;
    if (!std::filesystem::exists(filepath)) {
        return {};
    }
    const auto file = IOFile{patch_dir / filename, FileAccessMode::Read};
    std::vector<u32> code(file.GetSize() / sizeof(u32));
    file.Read(code);
    return code;
}
} // namespace Vulkan
