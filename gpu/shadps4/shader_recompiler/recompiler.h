// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/object_pool.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/program.h"

namespace Shader {

struct Profile;
struct RuntimeInfo;

struct Pools {
    static constexpr u32 InstPoolSize = 8192;
    static constexpr u32 BlockPoolSize = 32;

    Common::ObjectPool<IR::Inst> inst_pool;
    Common::ObjectPool<IR::Block> block_pool;

    explicit Pools() : inst_pool{InstPoolSize}, block_pool{BlockPoolSize} {}

    void ReleaseContents() {
        block_pool.ReleaseContents();
        inst_pool.ReleaseContents();
    }
};

// uc3: GOW3_UC3_MOVIE_FIX=1 with GOW3_UC3_RES=WxH (see recompiler.cpp). Uc3MovieFixKey() is 0 when
// off or at 1920x1080, otherwise a value that changes with the resolution (part of the shader
// cache version); Uc3MovieSize gives the resolution.
constexpr u64 Uc3MovieShaderHash = 0xc85d9816ull;
u32 Uc3MovieFixKey();
void Uc3MovieSize(u32& width, u32& height);

// uc3: GOW3_DOF_FOCUS_FIX=1 with GOW3_UC3_RES=WxH (see recompiler.cpp). DofFocusFixKey() is 0 when
// off or at 1920x1080, otherwise a value that changes with the resolution (in the shader's cache name).
constexpr u64 DofFocusShaderHash = 0x9f801363ull;
u32 DofFocusFixKey();

[[nodiscard]] IR::Program TranslateProgram(const std::span<const u32>& code, Pools& pools,
                                           Info& info, RuntimeInfo& runtime_info,
                                           const Profile& profile);

} // namespace Shader
