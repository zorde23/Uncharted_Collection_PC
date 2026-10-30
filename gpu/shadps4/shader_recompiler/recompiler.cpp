// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <string>
#include <vector>

#include "common/logging/classes.h"
#include "shader_recompiler/frontend/control_flow_graph.h"
#include "shader_recompiler/frontend/decode.h"
#include "shader_recompiler/frontend/structured_control_flow.h"
#include "shader_recompiler/frontend/translate/translate.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/post_order.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"

namespace Shader {

IR::BlockList GenerateBlocks(const IR::AbstractSyntaxList& syntax_list) {
    size_t num_syntax_blocks{};
    for (const auto& [_, type] : syntax_list) {
        if (type == IR::AbstractSyntaxNode::Type::Block) {
            ++num_syntax_blocks;
        }
    }
    IR::BlockList blocks{};
    blocks.reserve(num_syntax_blocks);
    for (const auto& [data, type] : syntax_list) {
        if (type == IR::AbstractSyntaxNode::Type::Block) {
            blocks.push_back(data.block);
        }
    }
    return blocks;
}

void EmitControlFlowGraph(IR::Program& program, Pools& pools, Gcn::CFG& cfg,
                          RuntimeInfo& runtime_info, const Profile& profile) {
    Gcn::Translator translator{program.info, runtime_info, profile};
    for (auto& block : cfg) {
        const u32 start = block.begin_index;
        const u32 size = block.end_index - start + 1;
        auto* ir_block = pools.block_pool.Create(pools.inst_pool);
        ir_block->cfg_block = &block;
        block.ir_block = ir_block;
        translator.Translate(ir_block, block.begin, block.cond,
                             std::span{program.ins_list}.subspan(start, size));
        program.blocks.push_back(ir_block);
    }
    translator.EmitPrologue(program.blocks.front());
    ASSERT_MSG(!program.info.translation_failed, "Shader translation has failed");
    for (auto& block : cfg) {
        auto* ir_block = block.ir_block;
        if (block.branch_true) {
            auto* true_block = block.branch_true->ir_block;
            ir_block->AddBranch(true_block);
        }
        if (block.branch_false) {
            auto* false_block = block.branch_false->ir_block;
            ir_block->AddBranch(false_block);
        }
    }
    program.post_order_blocks = Shader::IR::PostOrder(program.blocks.front());
}

// uc3: GOW3_UC3_MOVIE_FIX=1. The movie NV12->RGB compute (cs c85d9816) runs over the whole output
// and samples the decoded picture at ((x+0.5)/1920, (y+0.5)/1088), literals in its code. Above
// 1080p (GOW3_UC3_RES) the four literals are scaled to the output, so the picture covers it; the
// rasterizer points the shader's T#s back at the 1920x1080 picture (Uc3MovieSharp). Off by default.
static u32 movie_width = 1920, movie_height = 1080;
u32 Uc3MovieFixKey() {
    static const u32 key = [] {
        const char* on = std::getenv("GOW3_UC3_MOVIE_FIX");
        const char* res = std::getenv("GOW3_UC3_RES");
        unsigned w = 0, h = 0;
        if (!on || on[0] != '1' || !res || std::sscanf(res, "%ux%u", &w, &h) != 2 || w < 640 ||
            h < 360 || w > 7680 || h > 4320 || (w == 1920 && h == 1080)) {
            return 0u;
        }
        movie_width = w;
        movie_height = h;
        std::printf("Shader recompiler: movie picture scaled to %ux%u (GOW3_UC3_MOVIE_FIX)\n", w, h);
        return 0x20000u | (((w * 31u + h) & 0xfffu) << 18);
    }();
    return key;
}
void Uc3MovieSize(u32& width, u32& height) {
    Uc3MovieFixKey();
    width = movie_width;
    height = movie_height;
}
static std::vector<u32> Uc3MovieCode(std::span<const u32> code) {
    const auto bits = [](float f) {
        u32 u;
        std::memcpy(&u, &f, sizeof(u));
        return u;
    };
    u32 w, h;
    Uc3MovieSize(w, h);
    const float su = 1.0f / float(w), sv = (1.0f / 1088.0f) * (1080.0f / float(h));
    const std::pair<u32, u32> literals[] = {{0x3a088889u, bits(su)},
                                            {0x39888889u, bits(0.5f * su)},
                                            {0x3a70f0f1u, bits(sv)},
                                            {0x39f0f0f1u, bits(0.5f * sv)}};
    // Every literal is found in the original code before any is replaced: a new value can equal
    // another original one (1/3840 is 0.5/1920).
    std::vector<u32> out(code.begin(), code.end());
    size_t at[4];
    for (size_t i = 0; i < 4; ++i) {
        const u32 from = literals[i].first;
        if (std::count(code.begin(), code.end(), from) != 1) {
            std::printf("Shader recompiler: movie literal %08x not found once, shader left alone\n", from);
            return {};
        }
        at[i] = size_t(std::find(code.begin(), code.end(), from) - code.begin());
    }
    for (size_t i = 0; i < 4; ++i) {
        out[at[i]] = literals[i].second;
    }
    return out;
}

// uc3: GOW3_RES_LITERAL_LOG=1 (diagnostics, off): prints each shader whose code holds a 1080p size
// literal (1920, 1080, 960, 540, 480, 270 as floats or integers, the float inverses, 1/1088, 1/544), the kind that broke the
// movies above 1080p; a match can also be an instruction word, so it is a list to check, not a fix.
static void LogResolutionLiterals(std::span<const u32> code, u64 hash) {
    static const bool on = [] {
        const char* v = std::getenv("GOW3_RES_LITERAL_LOG");
        return v && v[0] == '1';
    }();
    if (!on) {
        return;
    }
    static const std::pair<u32, const char*> literals[] = {
        {0x3a088889u, "1/1920"}, {0x3a72b9d6u, "1/1080"}, {0x3a888889u, "1/960"}, {0x3af2b9d6u, "1/540"},
        {0x3b088889u, "1/480"},  {0x3b72b9d6u, "1/270"},  {0x3a70f0f1u, "1/1088"}, {0x3af0f0f1u, "1/544"},
        {0x44f00000u, "1920"},   {0x44870000u, "1080"},   {0x44700000u, "960"},    {0x44070000u, "540"},
        {0x43f00000u, "480"},    {0x43870000u, "270"},
        // as integers (texel coordinates): 1920 1080 960 540 480 270
        {0x780u, "int 1920"}, {0x438u, "int 1080"}, {0x3c0u, "int 960"}, {0x21cu, "int 540"},
        {0x1e0u, "int 480"},  {0x10eu, "int 270"}};
    std::string found;
    for (const auto& [bits, name] : literals) {
        if (std::find(code.begin(), code.end(), bits) != code.end()) {
            found += found.empty() ? name : std::string(" ") + name;
        }
    }
    if (!found.empty()) {
        std::printf("Shader recompiler: shader %016llx has 1080p literals: %s\n",
                    static_cast<unsigned long long>(hash), found.c_str());
    }
}

// uc3: GOW3_DOF_FOCUS_FIX=1 with GOW3_UC3_RES=WxH (off). The aim autofocus (ps 9f801363) takes the
// nearest depth in a window around a point the game gives in 1080p pixels times 1 / render size:
// at 2560x1440 the point (960, 540) became (0.375, 0.375), left of and above the crosshair, often on
// Drake, so the scene behind the crosshair blurred (worse at 4K, none at 1080p). Its constants 0 to 3
// (the point and the window's half size, 12 pixels) are scaled by render size / 1080p, as at 1080p.
static u32 dof_width = 1920, dof_height = 1080;
u32 DofFocusFixKey() {
    static const u32 key = [] {
        const char* on = std::getenv("GOW3_DOF_FOCUS_FIX");
        const char* res = std::getenv("GOW3_UC3_RES");
        unsigned w = 0, h = 0;
        if (!on || on[0] != '1' || !res || std::sscanf(res, "%ux%u", &w, &h) != 2 || w < 640 ||
            h < 360 || w > 7680 || h > 4320 || (w == 1920 && h == 1080)) {
            return 0u;
        }
        dof_width = w;
        dof_height = h;
        std::printf("Shader recompiler: aim focus point scaled to %ux%u (GOW3_DOF_FOCUS_FIX)\n", w, h);
        return w * 31u + h;
    }();
    return key;
}
static void DofFocusPatch(IR::Program& program, const Info& info) {
    if (info.pgm_hash != DofFocusShaderHash || info.sw_stage != SwStage::Fragment ||
        !DofFocusFixKey()) {
        return;
    }
    std::vector<IR::Inst*> casts;
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::BitCastF32U32) {
                continue;
            }
            const IR::Inst* read = inst.Arg(0).TryInst();
            if (!read || read->GetOpcode() != IR::Opcode::ReadConstBuffer ||
                !read->Arg(1).IsImmediate() || read->Arg(1).U32() > 3) {
                continue;
            }
            casts.push_back(&inst);
        }
    }
    if (casts.size() != 4) {
        std::printf("Shader recompiler: aim focus shader has %zu of its 4 constants, left alone\n",
                    casts.size());
        return;
    }
    const float sx = float(dof_width) / 1920.0f, sy = float(dof_height) / 1080.0f;
    for (IR::Inst* cast : casts) {
        const u32 index = cast->Arg(0).Inst()->Arg(1).U32();
        IR::IREmitter ir{*cast->GetParent(), IR::Block::InstructionList::s_iterator_to(*cast)};
        const IR::F32 value = ir.BitCast<IR::F32>(IR::U32{cast->Arg(0)});
        const IR::F32 scaled{ir.FPMul(value, ir.Imm32((index & 1) ? sy : sx))};
        cast->ReplaceUsesWith(scaled);
    }
    std::printf("Shader recompiler: aim focus shader %016llx scaled by %.3f x %.3f\n",
                static_cast<unsigned long long>(info.pgm_hash), sx, sy);
}

IR::Program TranslateProgram(const std::span<const u32>& code_in, Pools& pools, Info& info,
                             RuntimeInfo& runtime_info, const Profile& profile) {
    std::vector<u32> movie_code;
    if (info.pgm_hash == Uc3MovieShaderHash && Uc3MovieFixKey()) {
        movie_code = Uc3MovieCode(code_in);
    } else if (Uc3MovieFixKey() &&
               std::find(code_in.begin(), code_in.end(), 0x3a088889u) != code_in.end() &&
               std::find(code_in.begin(), code_in.end(), 0x3a70f0f1u) != code_in.end()) {
        // Another game's movie shader (UC2, UC1)? It holds 1/1920 and 1/1088 too.
        std::printf("Shader recompiler: shader %016llx has the movie literals 1/1920 and 1/1088, not "
                    "patched\n", static_cast<unsigned long long>(info.pgm_hash));
    }
    const std::span<const u32> code = movie_code.empty() ? code_in : std::span<const u32>(movie_code);
    LogResolutionLiterals(code_in, info.pgm_hash);
    // Ensure first instruction is expected.
    constexpr u32 token_mov_vcchi = 0xBEEB03FF;
    if (code[0] != token_mov_vcchi) {
        LOG_WARNING(Render_Recompiler, "First instruction is not s_mov_b32 vcc_hi, #imm");
    }

    Gcn::GcnCodeSlice slice(code.data(), code.data() + code.size());
    Gcn::GcnDecodeContext decoder;

    // Decode and save instructions
    IR::Program program{info};
    program.ins_list.reserve(code.size());
    while (!slice.atEnd()) {
        program.ins_list.emplace_back(decoder.decodeInstruction(slice));
    }

    // Clear any previous pooled data.
    pools.ReleaseContents();

    // Create control flow graph
    Common::ObjectPool<Gcn::Block> gcn_block_pool{64};
    Gcn::CFG cfg{gcn_block_pool, program.ins_list};
    EmitControlFlowGraph(program, pools, cfg, runtime_info, profile);

    // On NVIDIA GPUs HW interpolation of clip distance values seems broken, and we need to emulate
    // it with expensive discard in PS.
    Shader::InjectClipDistanceAttributes(program, runtime_info);

    // Run optimization passes on unstructured graph
    if (!profile.support_float64) {
        Shader::Optimization::LowerFp64ToFp32(program);
    }
    Shader::Optimization::SsaRewritePass(program);
    Shader::Optimization::ConstantPropagationPass(program.post_order_blocks);
    Shader::Optimization::ReadLaneEliminationPass(program);
    if (info.sw_stage == SwStage::TessellationControl) {
        Shader::Optimization::TessellationPreprocess(program, runtime_info);
        Shader::Optimization::HullShaderTransform(program, runtime_info);
    } else if (info.sw_stage == SwStage::TessellationEval) {
        Shader::Optimization::TessellationPreprocess(program, runtime_info);
        Shader::Optimization::DomainShaderTransform(program, runtime_info);
    }
    Shader::Optimization::RingAccessElimination(program, runtime_info);
    Shader::IR::DumpProgram(program, info, "pre-res-discover.");
    auto resources = Shader::Optimization::ResourceDiscoverPass(program, profile);
    Shader::Optimization::FlattenExtendedUserdataPass(program);
    Shader::IR::DumpProgram(program, info, "pre-res-patch.");
    Shader::Optimization::ResourcePatchingPass(program.info, resources, profile);
    Shader::Optimization::LowerBufferFormatToRaw(program);
    Shader::Optimization::SharedMemorySimplifyPass(program, profile);
    Shader::Optimization::SharedMemoryToStoragePass(program, runtime_info, profile);
    Shader::Optimization::LowerUserClipPlanes(program, runtime_info);
    Shader::Optimization::PhiSimplificationPass(program);
    Shader::Optimization::InverseBallotEliminationPass(program);
    Shader::IR::DumpProgram(program, info, "pre-lower-phi.");

    // Prepare for structurization by clearing flow graph and lowering phis
    for (auto* ir_block : program.blocks) {
        ir_block->imm_predecessors.clear();
        ir_block->imm_successors.clear();
        ir_block->ssa_state.Reset();
    }
    Shader::Optimization::LowerPhisToRegsPass(program);

    // Structurize control flow graph and create program.
    program.syntax_list = Shader::Gcn::BuildASL(pools, cfg, info);
    program.blocks = GenerateBlocks(program.syntax_list);
    program.post_order_blocks = Shader::IR::PostOrder(program.syntax_list.front().data.block);

    // Run optimization passes on structured graph
    Shader::Optimization::SsaRepairPass(program);
    Shader::Optimization::SsaRewritePass(program);
    Shader::Optimization::SharedMemoryBarrierPass(program, runtime_info, profile);
    Shader::Optimization::DeadCodeEliminationPass(program);
    Shader::Optimization::LowerWave64BallotPass(program, runtime_info, profile);
    Shader::Optimization::LowerHardwareIntrinsics(program);
    Shader::Optimization::ConstantPropagationPass(program.post_order_blocks);
    Shader::Optimization::DeadCodeEliminationPass(program);
    if (info.sw_stage == SwStage::Fragment && profile.needs_integer_interpolation_fix) {
        Shader::Optimization::InterpolatedIntegerPass(program);
    }
    DofFocusPatch(program, info);
    Shader::Optimization::CollectShaderInfoPass(program, profile);
    Shader::IR::DumpProgram(program, info);

    return program;
}

} // namespace Shader
