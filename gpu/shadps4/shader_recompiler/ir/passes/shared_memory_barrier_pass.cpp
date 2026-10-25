// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// gow3: the multi-wave barriers (GOW3_LDS_BARRIERS_MULTIWAVE, Uncharted 3: Sully's shirt stretched
// into flat triangles on NVIDIA, the cloth solvers 1c957b01 and 809864d3) are BilalReloaded's, from
// https://github.com/bc160201983/Uncharted-The-Nathan-Drake-Collection-PC-port (GPL v2), as
// SuperMedo's uncharted_pc carries them (commit 754ed2a).

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string_view>
#include <unordered_set>
#include <vector>
#include "shader_recompiler/ir/breadth_first_search.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"

namespace Shader::Optimization {

static bool IsLoadShared(const IR::Inst& inst) {
    return inst.GetOpcode() == IR::Opcode::LoadSharedU16 ||
           inst.GetOpcode() == IR::Opcode::LoadSharedU32 ||
           inst.GetOpcode() == IR::Opcode::LoadSharedU64;
}

static bool IsWriteShared(const IR::Inst& inst) {
    const IR::Opcode opcode = inst.GetOpcode();
    if (opcode >= IR::Opcode::SharedAtomicIAdd32 && opcode <= IR::Opcode::SharedAtomicCmpSwap64) {
        return !inst.Flags<bool>();
    }
    return opcode == IR::Opcode::WriteSharedU16 || opcode == IR::Opcode::WriteSharedU32 ||
           opcode == IR::Opcode::WriteSharedU64;
}

// Inserts barriers when a shared memory write and read occur in the same basic block.
static void EmitBarrierInBlock(IR::Block* block, bool subgroup = false) {
    const auto emit_barrier = [subgroup](IR::IREmitter& ir) {
        if (subgroup) {
            ir.SubgroupBarrier();
        } else {
            ir.Barrier();
        }
    };
    enum class BarrierAction : u32 {
        None,
        BarrierOnWrite,
        BarrierOnRead,
    };
    BarrierAction action{};
    for (IR::Inst& inst : block->Instructions()) {
        if (IsLoadShared(inst)) {
            if (action == BarrierAction::BarrierOnRead) {
                IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
                emit_barrier(ir);
            }
            action = BarrierAction::BarrierOnWrite;
            continue;
        }
        if (IsWriteShared(inst)) {
            if (action == BarrierAction::BarrierOnWrite) {
                IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
                emit_barrier(ir);
            }
            action = BarrierAction::BarrierOnRead;
        }
    }
    if (action != BarrierAction::None) {
        IR::IREmitter ir{*block, --block->end()};
        emit_barrier(ir);
    }
}

using NodeSet = std::unordered_set<const IR::Block*>;

static void EmitBarrierAtBlockStart(IR::Block* block) {
    auto insert_point = std::ranges::find_if_not(block->Instructions(), IR::IsPhi);
    IR::IREmitter ir{*block, insert_point};
    ir.Barrier();
}

static bool IsDivergent(const IR::U1& cond) {
    return IR::BreadthFirstSearch(cond, [](IR::Inst* inst) -> std::optional<bool> {
               if (inst->GetOpcode() == IR::Opcode::GetAttributeU32 &&
                   inst->Arg(0).Attribute() == IR::Attribute::LocalInvocationId) {
                   return true;
               }
               return std::nullopt;
           }) == true;
}

// BilalReloaded: with several GCN waves per group a condition can be uniform inside each wave
// (ballots, readfirstlane, loaded data) and still differ between waves: a group barrier inside it
// hangs the GPU. Only values built from constants, user data and the workgroup id count as uniform.
static bool IsGroupNonUniform(const IR::U1& cond) {
    using Op = IR::Opcode;
    return IR::BreadthFirstSearch(cond, [](IR::Inst* inst) -> std::optional<bool> {
               switch (inst->GetOpcode()) {
               case Op::GetAttribute:
               case Op::GetAttributeU1:
               case Op::GetAttributeU32:
                   if (inst->NumArgs() > 0 && inst->Arg(0).IsImmediate() &&
                       inst->Arg(0).Type() == IR::Type::Attribute &&
                       inst->Arg(0).Attribute() == IR::Attribute::WorkgroupId) {
                       return std::nullopt;
                   }
                   return true;
               case Op::GetExec:
               case Op::GetVcc:
               case Op::GetVccLo:
               case Op::GetVccHi:
               case Op::GetScc:
               case Op::GetScalarRegister:
               case Op::GetVectorRegister:
               case Op::GetVirtualRegister:
               case Op::MaskedBitCount32:
               case Op::LaneId:
               case Op::QuadBroadcast:
               case Op::ReadFirstLane:
               case Op::ReadLane:
               case Op::WriteLane:
               case Op::Ballot:
               case Op::BallotFindLsb:
               case Op::InverseBallot:
               case Op::GroupAny:
                   return true;
               default:
                   break;
               }
               const auto name = IR::NameOf(inst->GetOpcode());
               if (name.starts_with("LoadShared") || name.starts_with("SharedAtomic") ||
                   name.starts_with("LoadBuffer") || name.starts_with("BufferAtomic") ||
                   name.starts_with("Image") || name.starts_with("Load")) {
                   return true;
               }
               return std::nullopt;
           }) == true;
}

// Inserts a barrier after divergent conditional blocks to avoid undefined
// behavior when some threads write and others read from shared memory.
static void EmitBarrierInMergeBlock(const IR::AbstractSyntaxNode::Data& data,
                                    NodeSet& divergence_end, u32& divergence_depth,
                                    bool group_check = false,
                                    const std::function<bool(const IR::Block*)>& allowed = {},
                                    const NodeSet* barrier_regions = nullptr) {
    const IR::U1 cond = data.if_node.cond;
    if (barrier_regions && barrier_regions->contains(data.if_node.merge)) {
        return; // the game's own barrier inside: uniform
    }
    if (group_check ? IsGroupNonUniform(cond) : IsDivergent(cond)) {
        if (divergence_depth == 0 && (!allowed || allowed(data.if_node.merge))) {
            EmitBarrierAtBlockStart(data.if_node.merge);
        }
        ++divergence_depth;
        divergence_end.emplace(data.if_node.merge);
    }
}

// BilalReloaded: merge blocks of the ifs and loops whose body already holds one of the game's own
// barriers (s_barrier). The game runs those barriers in every wave, so the conditions around them
// are uniform across the group however they are computed (a loop exit read from LDS after a
// reduction). Uncharted 3's cloth solver re-wrote lds[0] at the top of the next iteration while
// slower warps still read the previous sum at the bottom.
static NodeSet FindBarrierRegions(const IR::AbstractSyntaxList& syntax_list) {
    using Type = IR::AbstractSyntaxNode::Type;
    NodeSet regions;
    std::vector<const IR::Block*> open;
    for (const IR::AbstractSyntaxNode& node : syntax_list) {
        switch (node.type) {
        case Type::If:
            open.push_back(node.data.if_node.merge);
            break;
        case Type::Loop:
            open.push_back(node.data.loop.merge);
            break;
        case Type::EndIf:
        case Type::Repeat: {
            const IR::Block* merge =
                node.type == Type::EndIf ? node.data.end_if.merge : node.data.repeat.merge;
            const auto it = std::ranges::find(open, merge);
            if (it != open.end()) {
                open.erase(it, open.end());
            }
            break;
        }
        case Type::Block:
            for (const IR::Inst& inst : node.data.block->Instructions()) {
                if (inst.GetOpcode() == IR::Opcode::Barrier) {
                    regions.insert(open.begin(), open.end());
                    break;
                }
            }
            break;
        default:
            break;
        }
    }
    return regions;
}

// A barrier inside a loop is invalid when different invocations leave on different iterations.
// Mark such loops so their shared-memory synchronization can be deferred to the merge block.
static NodeSet FindDivergentLoops(const IR::AbstractSyntaxList& syntax_list,
                                  bool group_check = false,
                                  const NodeSet* barrier_regions = nullptr) {
    NodeSet divergent_loops;
    const auto divergent = [group_check](const IR::U1& cond) {
        return group_check ? IsGroupNonUniform(cond) : IsDivergent(cond);
    };
    const auto game_barrier = [barrier_regions](const IR::Block* merge) {
        return barrier_regions && barrier_regions->contains(merge);
    };
    for (const IR::AbstractSyntaxNode& node : syntax_list) {
        switch (node.type) {
        case IR::AbstractSyntaxNode::Type::Repeat:
            if (divergent(node.data.repeat.cond) && !game_barrier(node.data.repeat.merge)) {
                divergent_loops.emplace(node.data.repeat.merge);
            }
            break;
        case IR::AbstractSyntaxNode::Type::Break:
            if (divergent(node.data.break_node.cond) && !game_barrier(node.data.break_node.merge)) {
                divergent_loops.emplace(node.data.break_node.merge);
            }
            break;
        default:
            break;
        }
    }
    return divergent_loops;
}

static constexpr u32 GcnSubgroupSize = 64;

// Hex hashes separated by commas; "all" = every shader.
static std::vector<u64> ParseHashes(const char* env) {
    if (std::string_view{env} == "all") {
        return {~0ULL};
    }
    std::vector<u64> list;
    for (const char* s = env; s && *s;) {
        char* next{};
        const u64 hash = std::strtoull(s, &next, 16);
        if (hash > 1) {
            list.push_back(hash);
        }
        s = next && *next == ',' ? next + 1 : nullptr;
    }
    return list;
}

// GCN shaders with groups of several 64-lane waves still pass data through LDS inside one wave
// without a barrier (the wave runs in lockstep). With 32-lane subgroups (NVIDIA) the two halves of
// such a wave are separate warps and race: Uncharted 3's cloth solver (cs 1c957b01, 1024 threads,
// warp-synchronous reductions) stretched Sully's shirt into flat triangles, never on an AMD GPU.
// GOW3_LDS_BARRIERS_MULTIWAVE: unset or 0 = off, 1 = the two cloth solvers, all = every multi-wave
// compute shader, or hex hashes separated by commas. Only matters on 32-lane GPUs.
bool MultiwaveLdsBarriers(u64 pgm_hash) {
    static const std::vector<u64> hashes = []() -> std::vector<u64> {
        const char* env = std::getenv("GOW3_LDS_BARRIERS_MULTIWAVE");
        if (!env || !*env || std::string_view{env} == "0") {
            return {};
        }
        if (std::string_view{env} == "1") {
            return {0x1c957b01ULL, 0x809864d3ULL};
        }
        return ParseHashes(env);
    }();
    return std::ranges::any_of(
        hashes, [pgm_hash](u64 hash) { return hash == ~0ULL || hash == pgm_hash; });
}

// Shaders whose ifs/loops around the game's own barriers count as uniform (FindBarrierRegions).
// Both cloth solvers need it (809864d3 has the same convergence loop as 1c957b01).
// GOW3_LDS_BARRIER_REGIONS: unset = the two cloth solvers, 0 = none, all, or hex hashes.
static bool BarrierRegionShader(u64 pgm_hash) {
    static const std::vector<u64> hashes = []() -> std::vector<u64> {
        const char* env = std::getenv("GOW3_LDS_BARRIER_REGIONS");
        if (!env) {
            return {0x1c957b01ULL, 0x809864d3ULL};
        }
        return ParseHashes(env);
    }();
    return std::ranges::any_of(
        hashes, [pgm_hash](u64 hash) { return hash == ~0ULL || hash == pgm_hash; });
}

// Group barriers for a compute shader of several 64-lane waves on a GPU with smaller subgroups
// (BilalReloaded). Returns false when the shader is left to the subgroup barriers.
static bool MultiwaveBarrierPass(IR::Program& program, u32 threadgroup_size, bool log_lds) {
    using Type = IR::AbstractSyntaxNode::Type;
    // An early return could leave waves short of a barrier added below: skip such shaders.
    for (size_t i = 0; i + 1 < program.syntax_list.size(); ++i) {
        if (program.syntax_list[i].type == Type::Return) {
            return false;
        }
    }
    const bool use_regions = BarrierRegionShader(program.info.pgm_hash);
    const NodeSet barrier_regions =
        use_regions ? FindBarrierRegions(program.syntax_list) : NodeSet{};
    const NodeSet divergent_loops =
        FindDivergentLoops(program.syntax_list, true, use_regions ? &barrier_regions : nullptr);
    const auto has_lds = [](const IR::Block* block) {
        for (const IR::Inst& inst : block->Instructions()) {
            if (IsLoadShared(inst) || IsWriteShared(inst)) {
                return true;
            }
        }
        return false;
    };
    // Only conditionals and loops that access LDS get a barrier at their merge block: NVIDIA hung
    // on one after a loop without LDS ops (cs 1c957b01, a loop the dispatches never entered).
    NodeSet lds_regions;
    {
        std::vector<const IR::Block*> open;
        for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
            if (node.type == Type::If) {
                open.push_back(node.data.if_node.merge);
            } else if (node.type == Type::Loop) {
                open.push_back(node.data.loop.merge);
            } else if (node.type == Type::EndIf || node.type == Type::Repeat) {
                const IR::Block* merge =
                    node.type == Type::EndIf ? node.data.end_if.merge : node.data.repeat.merge;
                const auto it = std::ranges::find(open, merge);
                if (it != open.end()) {
                    open.erase(it, open.end());
                }
            } else if (node.type == Type::Block && has_lds(node.data.block)) {
                lds_regions.insert(open.begin(), open.end());
            }
        }
    }
    const std::function<bool(const IR::Block*)> allowed_merge = [&](const IR::Block* merge) {
        return lds_regions.contains(merge);
    };
    u32 divergence_depth{};
    NodeSet divergence_end;
    u32 merges{}, blocks{};
    for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
        if (node.type == Type::EndIf) {
            if (divergence_end.contains(node.data.end_if.merge)) {
                --divergence_depth;
            }
            continue;
        }
        if (node.type == Type::If) {
            EmitBarrierInMergeBlock(node.data, divergence_end, divergence_depth, true,
                                    allowed_merge, use_regions ? &barrier_regions : nullptr);
            continue;
        }
        if (node.type == Type::Loop && divergent_loops.contains(node.data.loop.merge)) {
            ++divergence_depth;
            continue;
        }
        if (node.type == Type::Repeat && divergent_loops.contains(node.data.repeat.merge)) {
            ASSERT(divergence_depth > 0);
            --divergence_depth;
            if (divergence_depth == 0 && allowed_merge(node.data.repeat.merge)) {
                EmitBarrierAtBlockStart(node.data.repeat.merge);
                ++merges;
            }
            continue;
        }
        if (node.type == Type::Block) {
            if (divergence_depth == 0) {
                EmitBarrierInBlock(node.data.block);
                ++blocks;
            } else {
                // Inside divergent code a group barrier could hang: keep the order in the wave.
                EmitBarrierInBlock(node.data.block, true);
            }
        }
    }
    if (log_lds) {
        std::printf("LDS multi-wave barriers in shader %016llx (%u threads): regions %s, %u loop "
                    "merges, %u uniform blocks\n",
                    static_cast<unsigned long long>(program.info.pgm_hash), threadgroup_size,
                    use_regions ? "on" : "off", merges, blocks);
        std::fflush(stdout);
    }
    return true;
}

void SharedMemoryBarrierPass(IR::Program& program, const RuntimeInfo& runtime_info,
                             const Profile& profile) {
    if (program.info.hw_stage != HwStage::Compute) {
        return;
    }
    const auto& cs_info = runtime_info.hw.cs;
    const u32 shared_memory_size = cs_info.shared_memory_size;
    const u32 threadgroup_size =
        cs_info.workgroup_size[0] * cs_info.workgroup_size[1] * cs_info.workgroup_size[2];
    if (shared_memory_size == 0) {
        return;
    }
    // GOW3_LOG_LDS=1: compute shaders with shared memory, their group size and what this pass does.
    static const bool log_lds = [] {
        const char* env = std::getenv("GOW3_LOG_LDS");
        return env && env[0] == '1';
    }();
    const bool multiwave = profile.needs_lds_barriers &&
                           profile.subgroup_size < GcnSubgroupSize &&
                           threadgroup_size > GcnSubgroupSize &&
                           threadgroup_size % GcnSubgroupSize == 0 &&
                           MultiwaveLdsBarriers(program.info.pgm_hash);
    if (log_lds) {
        std::printf("LDS shader %016llx: group %ux%ux%u (%u threads), %u bytes shared, subgroup "
                    "%u, %s\n",
                    static_cast<unsigned long long>(program.info.pgm_hash),
                    cs_info.workgroup_size[0], cs_info.workgroup_size[1],
                    cs_info.workgroup_size[2], threadgroup_size, shared_memory_size,
                    profile.subgroup_size,
                    multiwave ? "multi-wave barriers"
                    : threadgroup_size == GcnSubgroupSize && profile.needs_lds_barriers
                        ? "group barriers"
                        : "subgroup barriers");
        std::fflush(stdout);
    }
    if (multiwave && MultiwaveBarrierPass(program, threadgroup_size, log_lds)) {
        return;
    }
    // The compiler can only omit barriers when the local workgroup size is the same as the HW
    // subgroup.
    if (threadgroup_size != GcnSubgroupSize || !profile.needs_lds_barriers) {
        // GCN waves can use shared memory between invocations without explicit barriers because
        // they run in lockstep. Host compilers can reorder these accesses, so keep their order
        // with subgroup memory barriers.
        for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
            if (node.type == IR::AbstractSyntaxNode::Type::Block) {
                EmitBarrierInBlock(node.data.block, true);
            }
        }
        return;
    }
    using Type = IR::AbstractSyntaxNode::Type;
    u32 divergence_depth{};
    NodeSet divergence_end;
    const NodeSet divergent_loops = FindDivergentLoops(program.syntax_list);
    for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
        if (node.type == Type::EndIf) {
            if (divergence_end.contains(node.data.end_if.merge)) {
                --divergence_depth;
            }
            continue;
        }
        // Check if branch depth is zero, we don't want to insert barrier in potentially divergent
        // code.
        if (node.type == Type::If) {
            EmitBarrierInMergeBlock(node.data, divergence_end, divergence_depth);
            continue;
        }
        if (node.type == Type::Loop && divergent_loops.contains(node.data.loop.merge)) {
            ++divergence_depth;
            continue;
        }
        if (node.type == Type::Repeat && divergent_loops.contains(node.data.repeat.merge)) {
            ASSERT(divergence_depth > 0);
            --divergence_depth;
            if (divergence_depth == 0) {
                EmitBarrierAtBlockStart(node.data.repeat.merge);
            }
            continue;
        }
        if (node.type == Type::Block && divergence_depth == 0) {
            EmitBarrierInBlock(node.data.block);
        }
    }
}

} // namespace Shader::Optimization
