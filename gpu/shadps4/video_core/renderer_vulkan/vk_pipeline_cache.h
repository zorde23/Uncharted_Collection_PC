// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <unordered_map>
#include <shared_mutex>
#include <variant>
#include <boost/container/static_vector.hpp>
#include <tsl/robin_map.h>
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/specialization.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "video_core/renderer_vulkan/vk_async_compiler.h"
#include "vulkan/vulkan.hpp"

template <>
struct std::hash<vk::ShaderModule> {
    std::size_t operator()(const vk::ShaderModule& module) const noexcept {
        return std::hash<size_t>{}(reinterpret_cast<size_t>((VkShaderModule)module));
    }
};

namespace AmdGpu {
class Liverpool;
}

namespace Serialization {
struct Archive;
}

namespace Shader {
struct Info;
}

namespace Vulkan {

class Instance;
class Scheduler;
class ShaderCache;
struct AsyncShaderCompilation;
struct AsyncGraphicsCompilation;

struct Program {
    struct Module {
        vk::ShaderModule module;
        Shader::StageSpecialization spec;
    };
    static constexpr size_t MaxPermutations = 8;
    using ModuleList = boost::container::small_vector<Module, MaxPermutations>;

    Shader::Info info;
    ModuleList modules{};
    size_t last_used = 0; ///< gow3: permutation of the previous lookup, compared first
    /// gow3: `info` as translated, for draw-preparation workers (they must not read `info`,
    /// whose user data the GPU thread rewrites every draw). Guarded by programs_mutex.
    std::unique_ptr<Shader::Info> info_template;
    std::shared_ptr<AsyncShaderCompilation> pending;
    bool compilation_failed = false;
    std::vector<Shader::StageSpecialization> failed_permutations;

    Program() = default;
    Program(Shader::HwStage stage, Shader::SwStage l_stage, Shader::ShaderParams params)
        : info{stage, l_stage, params} {}

    void AddPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec) {
        modules.emplace_back(module, std::move(spec));
    }

    void InsertPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec,
                      size_t perm_idx) {
        modules.resize(std::max(modules.size(), perm_idx + 1)); // <-- beware of realloc
        modules[perm_idx] = {module, std::move(spec)};
    }
};

struct DrawIndirectParams {
    u16 vertex_sgpr_offset;
    u32 instance_sgpr_offset;
    bool is_indirect;
};

} // namespace Vulkan

namespace AmdGpu {
union Regs;
}

namespace Vulkan {

/// gow3: state of one graphics/compute pipeline selection. The GPU thread owns one
/// (PipelineCache::sel); draw-preparation workers use their own with their register copies.
struct PipelineSelection {
    const AmdGpu::Regs* regs{};
    std::array<Shader::RuntimeInfo, MaxShaderStages> runtime_infos{};
    std::array<const Shader::Info*, MaxShaderStages> infos{};
    std::array<vk::ShaderModule, MaxShaderStages> modules{};
    std::optional<Shader::Gcn::FetchShaderData> fetch_shader{};
    GraphicsPipelineKey graphics_key{};
    bool motion = false;
    bool compilation_pending = false;
    DrawIndirectParams draw_indirect_params{};
    struct PrepWorker* worker{}; ///< set: read-only selection for a draw-preparation worker
};

/// gow3: a draw-preparation worker's own program state (see vk_draw_prep.h).
struct PrepWorker {
    struct Stage {
        const Program* program;
        u64 hash;
        Shader::HwStage hw_stage;
        VAddr pgm_base;
        const std::vector<u32>* flat;
    };
    // Node-based: stages keep pointers to these Infos while later stages are inserted.
    std::unordered_map<const Program*, Shader::Info> infos;
    boost::container::static_vector<Stage, MaxShaderStages> stages;
    std::vector<u64> read_pages; ///< R.21: guest pages the SRT walks read (unsorted)
    std::vector<u64> ptr_pages;  ///< R.10 A1 v8: of them, the table pointer pages
    std::vector<Shader::SrtGuestRun> guest_runs; ///< R.10 A1 v9: guest ranges the walks read
    std::vector<Shader::SrtCheck> checks; ///< R.22: values the SRT walks read
    std::vector<u8> check_bytes;          ///< R.10 A1: the bytes those runs read
    bool failed = false;
};

struct PreparedDraw;

class PipelineCache {
public:
    explicit PipelineCache(const Instance& instance, Scheduler& scheduler,
                           AmdGpu::Liverpool* liverpool, u32 sparse_page_shift);
    ~PipelineCache();

    /// Loads the pipelines of earlier sessions. gow3: called by the presenter once the window
    /// can show a loading screen; `progress(done, total)` follows the pipelines read.
    void WarmUp(const std::function<void(u32 done, u32 total)>& progress = {});
    /// gow3: the same load on low-priority threads while the game runs; the GPU thread takes
    /// the results a few at a time (PumpWarmUp). False when it cannot (zip archive store).
    bool StartBackgroundWarmUp();
    void StartOnDemand();
    void Sync();
    void ReportCompilerStats();
    /// uc3 R.20: GOW3_PREP_TRUST hit rate and divergences (GPU thread).
    void ReportPrepTrust();

    bool LoadComputePipeline(Serialization::Archive& ar);
    bool LoadGraphicsPipeline(Serialization::Archive& ar);
    bool LoadPipelineStage(Serialization::Archive& ar, size_t stage);

    const GraphicsPipeline* GetGraphicsPipeline(const DrawIndirectParams params = {},
                                                const PreparedDraw* prepared = nullptr);
    const GraphicsPipeline* GetGraphicsPipelineOnce(const DrawIndirectParams params,
                                                    const PreparedDraw* prepared);

    /// gow3: worker side of draw preparation: selects the pipeline key for `sel.regs` without
    /// creating anything. False when a program or permutation does not exist yet.
    bool PrepareGraphicsPipeline(PipelineSelection& sel);

    /// gow3: GPU-thread side: the pipeline for a prepared draw after checking that registers
    /// and flattened user data match; null to take the regular path.
    const GraphicsPipeline* TryPreparedPipeline(const PreparedDraw& prepared);
    void NotePrepTrustDivergence(const char* what, u64 hash, VAddr base);
    u64 prep_trust_hits = 0, prep_trust_misses = 0, prep_trust_divergences = 0;
    /// R.21 miss reasons: GPU leaves, wait/unnoted packet, big write, page write, too many pages.
    u64 prep_trust_why[7]{};
    u64 prep_trust_validated = 0;
    u64 prep_trust_wait_validated = 0; ///< R.10 A1: after a CPU WaitRegMem, trusted by value
    u64 prep_trust_wait_changed = 0;   ///< R.10 A1: after a CPU WaitRegMem, values changed
    u64 prep_trust_label_page = 0;     ///< R.10 A1: a met wait's label page written since the snapshot
    u64 prep_trust_label_unmet = 0;    ///< R.10 A1: ... and a met wait no longer holds
    u64 prep_trust_check_calls = 0, prep_trust_checks = 0; ///< R.10 A1: value compares, runs
    u64 prep_trust_check_bytes = 0, prep_trust_check_skips = 0; ///< bytes compared, compares deduped
    u64 prep_trust_stamp_validated[4]{}; ///< R.10 A1: trusted by value after why 1/2/3 ([0] after a wait)
    u64 prep_trust_stamp_changed[4]{};   ///< R.10 A1: values changed, by the same origin
    u64 prep_trust_pending = 0;        ///< R.10.17: GPU write still pending in the read set at A
    u64 prep_trust_pending_runs = 0, prep_trust_pending_cycles = 0; ///< runs tested, their cost
    u64 prep_trust_strict = 0;         ///< R.10.17: by value refused, a page in the read set written
    const PreparedDraw* last_value_ok = nullptr; ///< R.10 A1: the previous call's draw held by value
    /// R.10 A1 v8 (Fable 1a): the pipeline the last trusted apply found, with its key. Reset
    /// whenever a graphics pipeline is erased or replaced.
    const GraphicsPipeline* last_trusted_pipe = nullptr;
    u64 last_trusted_hash = 0;
    GraphicsPipelineKey last_trusted_key{};
    u64 prep_trust_pipe_reused = 0; ///< R.10 A1 v8: trusted applies that reused it
    /// R.10 A1 v8 (Fable, diagnostics): page write misses by the origin of the stamp (SrtOrigin),
    /// and whether the regular walk then gave the prepared flat (needless) or not (real).
    u64 prep_trust_pw_origin[8]{}, prep_trust_pw_needless[8]{}, prep_trust_pw_real[8]{};
    u64 prep_trust_pw_ptr = 0, prep_trust_pw_leaf = 0; ///< stamped page held a table pointer / leaves
    /// R.10 A1 v9: page writes resolved by their ranges: missed the reads (trusted), hit, unknown.
    u64 prep_trust_pw_missed[8]{}, prep_trust_pw_hit[8]{}, prep_trust_pw_unknown[8]{};

    /// gow3: the prepared draw the last GetGraphicsPipeline used, or null (regular path).
    [[nodiscard]] const PreparedDraw* UsedPrepared() const noexcept {
        return used_prepared;
    }

    const ComputePipeline* GetComputePipeline();

    using Result = std::tuple<const Shader::Info*, vk::ShaderModule,
                              std::optional<Shader::Gcn::FetchShaderData>, u64>;
    Result GetProgram(PipelineSelection& sel, Shader::HwStage stage, Shader::SwStage l_stage,
                      const Shader::ShaderParams& params, Shader::Backend::Bindings& binding);

    std::optional<vk::ShaderModule> ReplaceShader(vk::ShaderModule module,
                                                  std::span<const u32> spv_code);

    static std::string GetShaderName(Shader::HwStage stage, u64 hash,
                                     std::optional<size_t> perm = {});

    auto& GetProfile() const {
        return profile;
    }

private:
    bool QueueShader(Program& program, Shader::HwStage stage, Shader::SwStage l_stage,
                     const Shader::ShaderParams& params, Shader::RuntimeInfo runtime_info,
                     Shader::Backend::Bindings binding, size_t permutation, bool initial,
                     std::optional<Shader::StageSpecialization> specialization = {});
    void PublishShaders(bool wait = false);
    void PublishGraphics(bool wait = false);
    bool WaitPendingCompiles();
    std::chrono::steady_clock::time_point shader_wait_window{};
    std::chrono::steady_clock::duration shader_wait_spent{};
    void FinishCompilations();
    void SeedWorkerCaches();
    bool CheckCacheProfile();
    struct BackgroundWarmup;
    struct WarmStage;
    void ReadWarmEntry(BackgroundWarmup& warm, std::vector<u8>&& data);
    bool MergeWarmStage(WarmStage& stage);
    void PumpWarmUp();
    void StopWarmUp();
    bool AsyncCompilation() const;
    bool RefreshGraphicsKey(PipelineSelection& sel);
    bool RefreshGraphicsStages(PipelineSelection& sel);
    bool RefreshComputeKey();

    void DumpShader(std::span<const u32> code, u64 hash, Shader::HwStage stage, size_t perm_idx,
                    std::string_view ext);
    std::optional<std::vector<u32>> GetShaderPatch(u64 hash, Shader::HwStage stage, size_t perm_idx,
                                                   std::string_view ext);
    vk::ShaderModule CompileModule(Shader::Info& info, Shader::RuntimeInfo& runtime_info,
                                   const std::span<const u32>& code, size_t perm_idx,
                                   Shader::Backend::Bindings& binding);
    const Shader::RuntimeInfo& BuildRuntimeInfo(PipelineSelection& sel, Shader::HwStage stage,
                                                Shader::SwStage l_stage);

    [[nodiscard]] bool IsPipelineCacheDirty() const {
        return num_new_pipelines > 0;
    }

private:
    const Instance& instance;
    Scheduler& scheduler;
    AmdGpu::Liverpool* liverpool;
    DescriptorHeap desc_heap;
    vk::UniquePipelineCache pipeline_cache;
    vk::UniquePipelineLayout pipeline_layout;
    Shader::Profile profile{};
    Shader::Pools pools;
    CompilerPool<Shader::Pools> compiler_pools{8};
    std::unique_ptr<AsyncCompiler> compiler, capture_compiler;
    std::vector<vk::UniquePipelineCache> worker_pipeline_caches;
    bool worker_caches_seeded = false;
    /// gow3: GOW3_PARALLEL_WARMUP: WarmUp builds the cached pipelines on these threads.
    struct WarmupPool;
    WarmupPool* warmup_pool = nullptr;
    std::mutex warmup_results_mutex;
    std::vector<std::pair<GraphicsPipelineKey, std::unique_ptr<GraphicsPipeline>>> warmup_graphics;
    std::vector<std::pair<ComputePipelineKey, std::unique_ptr<ComputePipeline>>> warmup_compute;
    /// gow3: StartBackgroundWarmUp state; owned here, deleted by StopWarmUp.
    BackgroundWarmup* background = nullptr;
    tsl::robin_map<GraphicsPipelineKey, std::shared_ptr<AsyncGraphicsCompilation>> pending_graphics;
    tsl::robin_map<size_t, std::unique_ptr<Program>> program_cache;
    std::vector<Program*> pending_programs;
    /// gow3: exclusive for program/permutation insertions, shared for worker lookups.
    std::shared_mutex programs_mutex;
    u64 prepared_hits = 0, prepared_misses = 0;
    const PreparedDraw* used_prepared = nullptr;
    tsl::robin_map<ComputePipelineKey, std::unique_ptr<ComputePipeline>> compute_pipelines;
    tsl::robin_map<GraphicsPipelineKey, std::unique_ptr<GraphicsPipeline>> graphics_pipelines;
    PipelineSelection sel{}; ///< GPU thread selection state
    ComputePipelineKey compute_key{};
    u32 num_new_pipelines{}; // new pipelines added to the cache since the game start

    // Only if Config::collectShadersForDebug()
    tsl::robin_map<vk::ShaderModule,
                   std::vector<std::variant<GraphicsPipelineKey, ComputePipelineKey>>>
        module_related_pipelines;
};

} // namespace Vulkan
