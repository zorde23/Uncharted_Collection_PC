// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace AmdGpu {
struct ComputeProgram;
union Regs;
} // namespace AmdGpu

namespace Shader {
struct Info;
}

namespace Vulkan {

class Rasterizer;

/// uc3: shaders whose HLE writes guest memory: they run on the command processor thread, with
/// the draw pipe drained (GOW3_SCALAR_COPY_HLE).
bool IsGuestMemoryShaderHLE(unsigned long long pgm_hash);

/// uc3 (GOW3_HLE_INORDER=1): a scalar copy from a compute queue whose drain was skipped:
/// the destination write runs in order on the recording thread. False: drain and dispatch.
bool HleInOrderEnabled();
void NoteInOrderReject(unsigned reason);
/// uc3 (A4.1c): bytes a scalar copy dispatch would copy, 0 when it cannot tell.
unsigned long long ScalarCopyBytes(const Shader::Info& info, const AmdGpu::ComputeProgram& cs_program,
                                   unsigned long long* dst_address = nullptr);
unsigned long long BigPipeMin();
bool ExecuteScalarCopyInOrder(const Shader::Info& info, const AmdGpu::ComputeProgram& cs_program,
                              Rasterizer& rasterizer);

/// Attempts to execute a shader using HLE if possible.
bool ExecuteShaderHLE(const Shader::Info& info, const AmdGpu::Regs& regs,
                      const AmdGpu::ComputeProgram& cs_program, Rasterizer& rasterizer);

} // namespace Vulkan
