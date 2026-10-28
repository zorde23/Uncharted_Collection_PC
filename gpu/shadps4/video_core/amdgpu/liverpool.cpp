// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <mutex>
#include <unordered_map>
#include <pthread.h>
#ifndef _WIN32
#include <sys/resource.h>
#endif
#include <time.h>
#include "gow3_copy.h"
#include "gow3_toggles.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <boost/preprocessor/stringize.hpp>

#include "common/assert.h"
#include "common/debug.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/libraries/kernel/process.h"
#include "core/libraries/videoout/driver.h"
#include "core/memory.h"
#include "core/platform.h"
#include "video_core/amdgpu/liverpool.h"
#include "gow3_write_log.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/renderdoc.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"

namespace AmdGpu {

bool FrameGateStats() {
    static const bool on = [] {
        const char* env = std::getenv("GOW3_FRAME_GATE");
        return env && env[0] == '1';
    }();
    return on;
}

void NoteGateStage(GateStage stage) {
    if (!FrameGateStats()) {
        return;
    }
    using Clock = std::chrono::steady_clock;
    static std::mutex mutex;
    static Clock::time_point submit_done{}, decoded{}, wait_begin{}, last_print{};
    // Sums over the report window (ns) and frame count.
    static u64 sd_to_decoded = 0, decoded_to_drained = 0, sd_to_idle = 0, blocked = 0,
               blocked_waits = 0, frames = 0, guest_frame = 0;
    static Clock::time_point prev_submit_done{};
    const auto now = Clock::now();
    const auto ns = [](Clock::time_point a, Clock::time_point b) -> u64 {
        return a.time_since_epoch().count() && b > a
                   ? std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count()
                   : 0;
    };
    std::scoped_lock lk{mutex};
    switch (stage) {
    case GateStage::SubmitDone:
        guest_frame += ns(prev_submit_done, now);
        prev_submit_done = now;
        submit_done = now;
        decoded = {};
        break;
    case GateStage::Decoded:
        if (submit_done.time_since_epoch().count() && !decoded.time_since_epoch().count()) {
            decoded = now;
            sd_to_decoded += ns(submit_done, now);
        }
        break;
    case GateStage::Drained:
        if (decoded.time_since_epoch().count() && submit_done.time_since_epoch().count()) {
            decoded_to_drained += ns(decoded, now);
            sd_to_idle += ns(submit_done, now);
            ++frames;
            submit_done = {};
        }
        break;
    case GateStage::IdleWaitBegin:
        wait_begin = now;
        break;
    case GateStage::IdleWaitEnd: {
        const u64 waited = ns(wait_begin, now);
        if (waited > 20000) {
            blocked += waited;
            ++blocked_waits;
        }
        break;
    }
    }
    if (!last_print.time_since_epoch().count()) {
        last_print = now;
    }
    if (now - last_print >= std::chrono::seconds(5) && frames) {
        const double f = static_cast<double>(frames);
        std::printf("uc3 frame gate (5 s, %llu frames): guest frame %.2f ms; SubmitDone -> "
                    "decoded (stage A) %.2f ms, decoded -> drained (stage B + deferred fences) "
                    "%.2f ms, SubmitDone -> GpuIdle %.2f ms; guest blocked in WaitGpuIdle %.2f "
                    "ms/frame (%llu waits)\n",
                    static_cast<unsigned long long>(frames), guest_frame / 1e6 / f,
                    sd_to_decoded / 1e6 / f, decoded_to_drained / 1e6 / f, sd_to_idle / 1e6 / f,
                    blocked / 1e6 / f, static_cast<unsigned long long>(blocked_waits));
        sd_to_decoded = decoded_to_drained = sd_to_idle = blocked = blocked_waits = frames =
            guest_frame = 0;
        last_print = now;
    }
}

void NoteLabelWriteHook(u64 address, u64 size) {
    VideoCore::BufferCache::NoteLabelWrite(address, size);
}

void NoteGpuTimestamp(u64 address, u64 value, bool perf_counter) {
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_TIMESTAMP_DIAG");
        return env && env[0] == '1';
    }();
    if (!enabled) {
        return;
    }
    struct Entry {
        u64 previous = 0, count = 0, sum = 0, min = ~0ULL, max = 0;
        bool perf = false;
    };
    static std::mutex mutex;
    static std::unordered_map<u64, Entry> entries;
    static auto last_print = std::chrono::steady_clock::now();
    std::scoped_lock lk{mutex};
    auto& e = entries[address];
    e.perf = perf_counter;
    if (e.previous && value > e.previous) {
        const u64 delta = value - e.previous;
        ++e.count;
        e.sum += delta;
        e.min = std::min(e.min, delta);
        e.max = std::max(e.max, delta);
    }
    e.previous = value;
    const auto now = std::chrono::steady_clock::now();
    if (now - last_print < std::chrono::seconds(1)) {
        return;
    }
    last_print = now;
    // Deltas in ms: the clock is ns; the perf counter runs at the GPU core clock.
    const double perf_ms = 1000.0 / double(Libraries::GnmDriver::sceGnmGetGpuCoreClockFrequency());
    int printed = 0;
    for (auto& [addr, entry] : entries) {
        if (entry.count && printed++ < 8) {
            const double unit = entry.perf ? perf_ms : 1e-6;
            std::printf("uc3: GPU timestamp %#llx (%s): %llu writes/s, delta avg %.3f ms, min %.3f, "
                        "max %.3f\n",
                        static_cast<unsigned long long>(addr), entry.perf ? "perf" : "clock",
                        static_cast<unsigned long long>(entry.count), entry.sum * unit / entry.count,
                        entry.min * unit, entry.max * unit);
        }
        entry.count = entry.sum = entry.max = 0;
        entry.min = ~0ULL;
    }
}

static const char* dcb_task_name{"DCB_TASK"};
static const char* ccb_task_name{"CCB_TASK"};

#define MAX_NAMES 56
static_assert(Liverpool::NumComputeRings <= MAX_NAMES);

#define NAME_NUM(z, n, name) BOOST_PP_STRINGIZE(name) BOOST_PP_STRINGIZE(n),
#define NAME_ARRAY(name, num) {BOOST_PP_REPEAT(num, NAME_NUM, name)}

static const char* acb_task_name[] = NAME_ARRAY(ACB_TASK, MAX_NAMES);

#define YIELD(name)                                                                                \
    FIBER_EXIT;                                                                                    \
    co_yield {};                                                                                   \
    FIBER_ENTER(name);

#define YIELD_CE() YIELD(ccb_task_name)
#define YIELD_GFX() YIELD(dcb_task_name)
#define YIELD_ASC(id) YIELD(acb_task_name[id])

#define RESUME(task, name)                                                                         \
    FIBER_EXIT;                                                                                    \
    task.handle.resume();                                                                          \
    FIBER_ENTER(name);

#define RESUME_CE(task) RESUME(task, ccb_task_name)
#define RESUME_GFX(task) RESUME(task, dcb_task_name)
#define RESUME_ASC(task, id) RESUME(task, acb_task_name[id])

std::array<u8, 48_KB> Liverpool::ConstantEngine::constants_heap;

static std::span<const u32> NextPacket(std::span<const u32> span, size_t offset) {
    if (offset > span.size()) {
        LOG_ERROR(
            Lib_GnmDriver,
            ": packet length exceeds remaining submission size. Packet dword count={}, remaining "
            "submission dwords={}",
            offset, span.size());
        // Return empty subspan so check for next packet bails out
        return {};
    }

    return span.subspan(offset);
}

Liverpool::Liverpool() : guest_markers_enabled{EmulatorSettings.IsVkGuestMarkersEnabled()} {
    num_counter_pairs = Libraries::Kernel::sceKernelIsNeoMode() ? 16 : 8;
    process_thread = std::jthread{std::bind_front(&Liverpool::Process, this)};
}

Liverpool::~Liverpool() {
    process_thread.request_stop();
    process_thread.join();
}

// uc3 A4.2 (GOW3_CMD_INORDER=1, off by default): a command (a guest fault's readback) runs on
// the draw recording thread at its place in the draw pipe instead of stage A draining it first.
// Same order guarantee (everything queued before it is recorded), and stage A keeps going.
static bool CmdInOrder() {
    static const bool on = [] {
        const char* env = std::getenv("GOW3_CMD_INORDER");
        return env && env[0] == '1';
    }();
    return on;
}

static void RunCommandTask(Vulkan::Rasterizer&, const u8* data) {
    Common::UniqueFunction<void>* callback;
    std::memcpy(&callback, data, sizeof(callback));
    {
        Gow3Stats::Timer timer{Gow3Stats::cmd_ns};
        Gow3Stats::cmd_count.fetch_add(1, std::memory_order_relaxed);
        (*callback)();
    }
    delete callback;
}

void Liverpool::ProcessCommands() {
    if (rasterizer && CmdInOrder()) {
        while (num_commands) {
            auto* callback = new Common::UniqueFunction<void>{};
            {
                std::scoped_lock lk{submit_mutex};
                *callback = std::move(command_queue.front());
                command_queue.pop();
                --num_commands;
            }
            // false: the pipe was off, so it drained and ran here (and deleted it).
            rasterizer->RunInOrder(&RunCommandTask, &callback, sizeof(callback));
        }
        return;
    }
    // gow3: commands touch the caches, so the draw recording thread runs dry first.
    // uc3: drained inside the loop, once a command is really there. Checking num_commands
    // before the loop and again in its condition let a command that arrived in between (a
    // guest page fault's readback: ~150/s with precise readbacks) run with stage B still
    // recording a draw: both threads used the scheduler's record chunk, a null dereference.
    bool drained = false;
    // Process incoming commands with high priority
    while (num_commands) {
        if (!drained && rasterizer) {
            rasterizer->DrainDrawPipe(Vulkan::DrawPipe::ReasonCommands);
            drained = true;
        }
        Common::UniqueFunction<void> callback{};
        {
            std::scoped_lock lk{submit_mutex};
            callback = std::move(command_queue.front());
            command_queue.pop();
            --num_commands;
        }
        Gow3Stats::Timer timer{Gow3Stats::cmd_ns};
        Gow3Stats::cmd_count.fetch_add(1, std::memory_order_relaxed);
        callback();
    }
}

void Liverpool::Process(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuCommandProcessor");
#ifndef _WIN32
    if (clockid_t clock; pthread_getcpuclockid(pthread_self(), &clock) == 0) {
        Gow3Stats::gpu_thread_clock.store(static_cast<int>(clock));
    }
#else
    Gow3Stats::gpu_thread_id.store(GetCurrentThreadId());
#endif
    gpu_id = std::this_thread::get_id();
#if defined(__linux__) || defined(_WIN32)
    gpu_tid = gettid();
#endif

    while (!stoken.stop_requested()) {
        {
            const auto idle_start = std::chrono::steady_clock::now();
            std::unique_lock lk{submit_mutex};
            Common::CondvarWait(submit_cv, lk, stoken,
                                [this] { return num_commands || num_submits || submit_done; });
            Gow3Stats::gpu_idle_ns.fetch_add(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - idle_start).count(),
                std::memory_order_relaxed);
        }
        if (stoken.stop_requested()) {
            break;
        }

        VideoCore::StartCapture();

        curr_qid = -1;

        while (num_submits || num_commands) {
            ProcessCommands();

            curr_qid = (curr_qid + 1) % num_mapped_queues;

            auto& queue = mapped_queues[curr_qid];

            Task::Handle task{};
            {
                std::scoped_lock lock{queue.m_access};
                if (queue.submits.empty()) {
                    continue;
                }
                task = queue.submits.front();
            }
            task.resume();

            if (task.done()) {
                task.destroy();

                std::scoped_lock lock{queue.m_access};
                queue.submits.pop();

                --num_submits;
                std::scoped_lock lock2{submit_mutex};
                submit_cv.notify_all();
            }
        }

        if (submit_done) {
            NoteGateStage(GateStage::Decoded);
            VideoCore::EndCapture();
            if (rasterizer) {
                rasterizer->OnSubmit();
                rasterizer->Flush();
            }
            submit_done = false;
        }

        // gow3: the guest takes GPU idle (sceGnmSubmitDone) as its work being done and frees
        // the objects that hold its fence labels. The labels must be written before: the draw
        // recording thread and the fences it deferred to the Vulkan recording thread
        // (RecorderFences). Else a late fence write lands in freed memory (a corrupted guest
        // heap free list after minutes of play).
        // The same holds when sceGnmSubmitDone finds the GPU idle and does not block at all:
        // decoded (num_submits 0) is not done until then (work_retired).
        if (rasterizer) {
            rasterizer->DrainDrawPipe(Vulkan::DrawPipe::ReasonSubmissionEnd);
            rasterizer->WaitDeferredSignals();
        }
        NoteGateStage(GateStage::Drained);
        {
            std::scoped_lock lk{submit_mutex};
            if (num_submits == 0) {
                work_retired = true;
            }
        }
        Platform::IrqC::Instance()->Signal(Platform::InterruptId::GpuIdle);
    }
}

namespace {
/// gow3: packets the GPU command thread handles while the draw recording thread may still
/// work on earlier draws (register state, draws it hands over, nested buffers). Every other
/// packet first waits for that thread to run dry (Rasterizer::DrainDrawPipe).
bool PipelinedOpcode(PM4ItOpcode opcode) {
    switch (opcode) {
    case PM4ItOpcode::Nop:
    case PM4ItOpcode::ContextControl:
    case PM4ItOpcode::ClearState:
    case PM4ItOpcode::SetConfigReg:
    case PM4ItOpcode::SetContextReg:
    case PM4ItOpcode::SetShReg:
    case PM4ItOpcode::SetUconfigReg:
    case PM4ItOpcode::IndexType:
    case PM4ItOpcode::NumInstances:
    case PM4ItOpcode::IndexBase:
    case PM4ItOpcode::IndexBufferSize:
    case PM4ItOpcode::SetBase:
    case PM4ItOpcode::DrawIndex2:
    case PM4ItOpcode::DrawIndexOffset2:
    case PM4ItOpcode::DrawIndexAuto:
    case PM4ItOpcode::IndirectBuffer:
    case PM4ItOpcode::AcquireMem:
    case PM4ItOpcode::PfpSyncMe:
    case PM4ItOpcode::IncrementDeCounter:
    case PM4ItOpcode::WaitOnCeCounter:
    case PM4ItOpcode::EventWrite:     // occlusion results only, no rasterizer state
    case PM4ItOpcode::EventWriteEop:  // runs in order on the recording thread (RunInOrder)
    case PM4ItOpcode::EventWriteEos:
    case PM4ItOpcode::WaitRegMem:     // waits for the recording thread only while unmet
    case PM4ItOpcode::DispatchDirect: // handed over like draws (PipelinedDispatch)
    case PM4ItOpcode::DrawIndirect:   // likewise (PipelinedIndirectDraws; else drains itself)
    case PM4ItOpcode::DrawIndirectMulti:
    case PM4ItOpcode::DrawIndexIndirect:
    case PM4ItOpcode::DrawIndexIndirectMulti:
    case PM4ItOpcode::DrawIndexIndirectCountMulti:
    case PM4ItOpcode::DispatchIndirect:
    case PM4ItOpcode::WriteData:     // run in order on the recording thread (RunInOrder)
    case PM4ItOpcode::DmaData:
    case PM4ItOpcode::SetPredication: // not implemented (ignored): nothing to wait for. God of
                                      // War III sets it around most draws; draining for it was
                                      // ~60% of the GPU thread's time (15-30k drains/s).
        return true;
    default:
        return false;
    }
}

/// uc3 (Fable R.8.3): ACB packets that need no drain: registers, nested buffers, waits that
/// yield, and dispatches (handed over like draws, or draining themselves).
bool ComputePipelinedOpcode(PM4ItOpcode opcode) {
    switch (opcode) {
    case PM4ItOpcode::Nop:
    case PM4ItOpcode::SetShReg:
    case PM4ItOpcode::SetQueueReg:
    case PM4ItOpcode::AcquireMem:
    case PM4ItOpcode::IndirectBuffer:
    case PM4ItOpcode::Rewind:
    case PM4ItOpcode::WaitRegMem:
    case PM4ItOpcode::EventWrite:
    case PM4ItOpcode::DispatchDirect:
    case PM4ItOpcode::DispatchIndirect:
    case PM4ItOpcode::ReleaseMem: // runs in order on the recording thread (RunReleaseMem)
        return true;
    default:
        return false;
    }
}

/// ucndc (GOW3_INDIRECT_IB_FIX=1, off by default): indirect indexed draws set max_index_size
/// (the index buffer capacity) without marking it, so the draw recording thread kept the
/// value of the last direct draw. Candidate for the UC3 cave torch light (GPU-culled draws).
bool IndirectIbFix() {
    static const bool enabled = [] {
        const char* value = std::getenv("GOW3_INDIRECT_IB_FIX");
        return value && value[0] == '1';
    }();
    return enabled;
}

void MarkIndirectIndexSize(AmdGpu::Regs& regs, AmdGpu::RegDirty& dirty) {
    if (IndirectIbFix()) {
        dirty.MarkField(regs, regs.max_index_size);
    }
}

bool ComputeDrainLazy() {
    static const bool enabled = [] {
        const char* value = std::getenv("GOW3_COMPUTE_DRAIN_LAZY");
        return value && value[0] == '1';
    }();
    return enabled;
}

/// Compute packets that touch no cache or guest memory (GOW3_COMPUTE_DRAIN_LAZY).
bool ComputeRegisterOpcode(PM4ItOpcode opcode) {
    switch (opcode) {
    case PM4ItOpcode::Nop:
    case PM4ItOpcode::SetShReg:
    case PM4ItOpcode::SetQueueReg:
    case PM4ItOpcode::IndirectBuffer:
    case PM4ItOpcode::Rewind:
        return true;
    default:
        return false;
    }
}

/// uc3 R.10 A1 (R.22 §1): the guest range a memory-writing packet writes, stamped on the
/// command thread instead of a global change. False when the range is not known.
bool SrtNotePacketRange(const PM4Type3Header* header) {
    const auto stamp = [](VAddr address, u64 size) {
        Vulkan::SrtNoteWrite(address, size, Vulkan::SrtOriginPacket);
        return true;
    };
    const auto label_size = [](DataSelect sel) -> u32 {
        switch (sel) {
        case DataSelect::None:
            return 0;
        case DataSelect::Data32Low:
            return 4;
        case DataSelect::Data64:
        case DataSelect::GpuClock64:
        case DataSelect::PerfCounter:
            return 8;
        default:
            return ~0u;
        }
    };
    switch (header->opcode.Value()) {
    case PM4ItOpcode::EventWriteEop: {
        const auto* eop = reinterpret_cast<const PM4CmdEventWriteEop*>(header);
        const u32 size = label_size(eop->data_sel.Value());
        return size == ~0u ? false : size == 0 ? true : stamp(reinterpret_cast<VAddr>(eop->Address<u32>()), size);
    }
    case PM4ItOpcode::EventWriteEos: {
        const auto* eos = reinterpret_cast<const PM4CmdEventWriteEos*>(header);
        return stamp(eos->Address<VAddr>(), 4);
    }
    case PM4ItOpcode::ReleaseMem: {
        const auto* release = reinterpret_cast<const PM4CmdReleaseMem*>(header);
        if (release->data_sel.Value() == DataSelect::GdsMemStore) {
            return stamp(release->Address<VAddr>(), release->num_dw * sizeof(u32));
        }
        const u32 size = label_size(release->data_sel.Value());
        return size == ~0u ? false : size == 0 ? true : stamp(release->Address<VAddr>(), size);
    }
    case PM4ItOpcode::DmaData: {
        const auto* dma = reinterpret_cast<const PM4DmaData*>(header);
        if (dma->dst_sel == DmaDataDst::Memory || dma->dst_sel == DmaDataDst::MemoryUsingL2) {
            return stamp(dma->DstAddress<VAddr>(), dma->NumBytes());
        }
        return dma->dst_sel == DmaDataDst::Gds;
    }
    case PM4ItOpcode::WriteData: {
        const auto* write = reinterpret_cast<const PM4CmdWriteData*>(header);
        return stamp(write->Address<VAddr>(), (header->count.Value() - 2) * sizeof(u32));
    }
    case PM4ItOpcode::CopyData:
        return true; // not executed (logged as unhandled)
    default:
        return false;
    }
}

bool ComputePipeOn() {
    static const bool enabled = [] {
        const char* value = std::getenv("GOW3_COMPUTE_PIPE");
        return value && value[0] == '1';
    }();
    return enabled;
}
} // namespace

void Liverpool::SrtNoteGpuLabelPacket(const PM4Header* header, u64 seq) {
    switch (header->type3.opcode.Value()) {
    case PM4ItOpcode::EventWriteEop: {
        const auto* eop = reinterpret_cast<const PM4CmdEventWriteEop*>(header);
        const auto address = reinterpret_cast<VAddr>(eop->Address<u32>());
        if (eop->data_sel.Value() == DataSelect::Data32Low) {
            Vulkan::SrtNoteGpuLabel(address, eop->DataDWord(), seq);
        } else if (eop->data_sel.Value() == DataSelect::Data64) {
            Vulkan::SrtNoteGpuLabel(address, eop->DataQWord(), seq);
        }
        break;
    }
    case PM4ItOpcode::EventWriteEos: {
        const auto* eos = reinterpret_cast<const PM4CmdEventWriteEos*>(header);
        if (eos->command.Value() == PM4CmdEventWriteEos::Command::SignalFence) {
            Vulkan::SrtNoteGpuLabel(eos->Address<VAddr>(), eos->DataDWord(), seq);
        }
        break;
    }
    case PM4ItOpcode::ReleaseMem: {
        const auto* release = reinterpret_cast<const PM4CmdReleaseMem*>(header);
        if (release->data_sel.Value() == DataSelect::Data32Low) {
            Vulkan::SrtNoteGpuLabel(release->Address<VAddr>(), release->DataDWord(), seq);
        } else if (release->data_sel.Value() == DataSelect::Data64) {
            Vulkan::SrtNoteGpuLabel(release->Address<VAddr>(), release->DataQWord(), seq);
        }
        break;
    }
    case PM4ItOpcode::WriteData: {
        const auto* write = reinterpret_cast<const PM4CmdWriteData*>(header);
        const u32 dwords = header->type3.count.Value() - 2;
        if (dwords == 1) {
            Vulkan::SrtNoteGpuLabel(write->Address<VAddr>(), write->data[0], seq);
        } else if (dwords == 2) {
            Vulkan::SrtNoteGpuLabel(write->Address<VAddr>(),
                                    write->data[0] | u64(write->data[1]) << 32, seq);
        }
        break;
    }
    default:
        break;
    }
}

namespace {
void SignalEop(const PM4CmdEventWriteEop& eop) {
    eop.SignalFence(
        [](void* address, u64 data, u32 num_bytes) {
            auto* memory = Core::Memory::Instance();
            Gow3WriteLog::Note(reinterpret_cast<u64>(address), &data, num_bytes, Gow3WriteLog::Fence);
            VideoCore::BufferCache::NoteLabelWrite(reinterpret_cast<VAddr>(address), num_bytes);
            if (!memory->TryWriteBacking(address, &data, num_bytes)) {
                memcpy(address, &data, num_bytes);
            }
            Vulkan::SrtNoteLanding(reinterpret_cast<VAddr>(address), num_bytes);
        },
        [] { Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxEop); });
}

// gow3: end-of-pipe/-shader events run in order with the draws (Rasterizer::RunInOrder: on
// the draw recording thread when the draw pipeline is in use).
void RunEventWriteEop(Vulkan::Rasterizer& rasterizer, const u8* data) {
    const auto eop = *reinterpret_cast<const PM4CmdEventWriteEop*>(data);
    rasterizer.ProcessDownloadImages();
    // Guest memory copies on the copy threads precede the fence the guest sees.
    // GOW3_ASYNC_FENCES=1 writes it once they are done instead of waiting for them; measured
    // slower (79 vs 83 FPS): the guest waits on these fences, and later fences stall it more
    // than the wait costs.
    static const bool async_fences = [] {
        const char* env = std::getenv("GOW3_ASYNC_FENCES");
        return env && env[0] == '1';
    }();
    if (async_fences && !Gow3Toggle::Disabled(Gow3Toggle::AsyncFences)) {
        Gow3Copy::AfterCopies([eop] { SignalEop(eop); });
    } else if (Vulkan::DrawPipe::OnStageB() && !Gow3Toggle::Disabled(Gow3Toggle::RecorderFences)) {
        // The draw recording thread does not wait: the Vulkan recording thread signals the
        // fence after the copies queued before it.
        rasterizer.SignalAfterHostCopies([eop] { SignalEop(eop); });
    } else {
        // Labels are written in order: not before the ones deferred to the recording thread.
        rasterizer.WaitHostCopies();
        rasterizer.WaitDeferredSignals();
        SignalEop(eop);
    }
}

void RunDmaData(Vulkan::Rasterizer& rasterizer, const u8* data) {
    const auto* dma_data = reinterpret_cast<const PM4DmaData*>(data);
    ASSERT(dma_data->command.das == 0);
    if (dma_data->src_sel == DmaDataSrc::Data && dma_data->dst_sel == DmaDataDst::Gds) {
        rasterizer.FillBuffer(dma_data->dst_addr_lo, dma_data->NumBytes(),
                               dma_data->data, true);
    } else if ((dma_data->src_sel == DmaDataSrc::Memory ||
                dma_data->src_sel == DmaDataSrc::MemoryUsingL2) &&
               dma_data->dst_sel == DmaDataDst::Gds) {
        rasterizer.CopyBuffer(dma_data->dst_addr_lo, dma_data->SrcAddress<VAddr>(),
                               dma_data->NumBytes(), true, false);
    } else if (dma_data->src_sel == DmaDataSrc::Data &&
               (dma_data->dst_sel == DmaDataDst::Memory ||
                dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
        rasterizer.FillBuffer(dma_data->DstAddress<VAddr>(), dma_data->NumBytes(),
                               dma_data->data, false);
    } else if (dma_data->src_sel == DmaDataSrc::Gds &&
               (dma_data->dst_sel == DmaDataDst::Memory ||
                dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
        rasterizer.CopyBuffer(dma_data->DstAddress<VAddr>(), dma_data->src_addr_lo,
                               dma_data->NumBytes(), false, true);
    } else if ((dma_data->src_sel == DmaDataSrc::Memory ||
                dma_data->src_sel == DmaDataSrc::MemoryUsingL2) &&
               (dma_data->dst_sel == DmaDataDst::Memory ||
                dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
        rasterizer.CopyBuffer(dma_data->DstAddress<VAddr>(),
                               dma_data->SrcAddress<VAddr>(), dma_data->NumBytes(),
                               false, false);
    } else {
        UNREACHABLE_MSG("WriteData src_sel = {}, dst_sel = {}", u32(dma_data->src_sel),
                        u32(dma_data->dst_sel));
    }
    if (dma_data->dst_sel == DmaDataDst::Memory || dma_data->dst_sel == DmaDataDst::MemoryUsingL2) {
        Vulkan::SrtNoteLanding(dma_data->DstAddress<VAddr>(), dma_data->NumBytes());
    }
}

void SignalFlip(Vulkan::Rasterizer& rasterizer, const u8*) {
    rasterizer.WaitDeferredSignals();
    Gow3Stats::NoteFlipStage(2);
    Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxFlip);
}

void RunWriteData(Vulkan::Rasterizer& rasterizer, const u8* data) {
    const auto* header = reinterpret_cast<const PM4Header*>(data);
    const auto* write_data = reinterpret_cast<const PM4CmdWriteData*>(data);
    // Copies deferred to the recording thread, and fences deferred to it (RecorderFences),
    // precede writes the guest sees.
    rasterizer.WaitHostCopies();
    rasterizer.WaitDeferredSignals();
    Gow3WriteLog::Note(write_data->Address<u64>(), write_data->data,
                     (header->type3.count.Value() - 2) * sizeof(u32), Gow3WriteLog::WriteData);
    std::memcpy(write_data->Address<u64*>(), write_data->data,
                (header->type3.count.Value() - 2) * sizeof(u32));
    Vulkan::SrtNoteLanding(write_data->Address<VAddr>(),
                           (header->type3.count.Value() - 2) * sizeof(u32));
}

void RunEventWriteEos(Vulkan::Rasterizer& rasterizer, const u8* data) {
    const auto& event_eos = *reinterpret_cast<const PM4CmdEventWriteEos*>(data);
    // Copies deferred to the recording thread precede writes the guest sees.
    rasterizer.WaitHostCopies();
    rasterizer.WaitDeferredSignals();
    rasterizer.ProcessDownloadImages();
    event_eos.SignalFence([](void* address, u64 value, u32 num_bytes) {
        auto* memory = Core::Memory::Instance();
        Gow3WriteLog::Note(reinterpret_cast<u64>(address), &value, num_bytes, Gow3WriteLog::Fence);
        VideoCore::BufferCache::NoteLabelWrite(reinterpret_cast<VAddr>(address), num_bytes);
        if (!memory->TryWriteBacking(address, &value, num_bytes)) {
            memcpy(address, &value, num_bytes);
        }
        Vulkan::SrtNoteLanding(reinterpret_cast<VAddr>(address), num_bytes);
    });
    if (event_eos.command == PM4CmdEventWriteEos::Command::GdsStore) {
        ASSERT(event_eos.size == 1);
        rasterizer.Finish();
        const u32 value = rasterizer.ReadDataFromGds(event_eos.gds_index);
        *event_eos.Address() = value;
    }
}

/// uc3 (Fable R.8.3, GOW3_COMPUTE_PIPE=1): a compute ReleaseMem runs in order with the draws
/// on the recording thread, like EventWriteEop, instead of draining the pipe first.
struct ReleaseMemTask {
    PM4CmdReleaseMem packet;
    u32 pipe_id;
};

void SignalReleaseMem(Vulkan::Rasterizer& rasterizer, const ReleaseMemTask& task) {
    const auto& packet = task.packet;
    const auto write = [&](auto value) {
        void* address = packet.Address<void*>();
        Gow3WriteLog::Note(reinterpret_cast<u64>(address), &value, sizeof(value),
                           Gow3WriteLog::Fence);
        VideoCore::BufferCache::NoteLabelWrite(reinterpret_cast<VAddr>(address), sizeof(value));
        if (!Core::Memory::Instance()->TryWriteBacking(address, &value, sizeof(value))) {
            std::memcpy(address, &value, sizeof(value));
        }
        Vulkan::SrtNoteLanding(reinterpret_cast<VAddr>(address), sizeof(value));
    };
    switch (packet.data_sel.Value()) {
    case DataSelect::Data32Low:
        write(packet.DataDWord());
        break;
    case DataSelect::Data64:
        write(packet.DataQWord());
        break;
    case DataSelect::GpuClock64:
        { const u64 ts = GetGpuClock64(); NoteGpuTimestamp(packet.Address<u64>(), ts, false); write(ts); }
        break;
    case DataSelect::PerfCounter:
        { const u64 ts = GetGpuPerfCounter(); NoteGpuTimestamp(packet.Address<u64>(), ts, true); write(ts); }
        break;
    case DataSelect::GdsMemStore:
        rasterizer.CopyBuffer(packet.Address<VAddr>(), packet.gds_index,
                              packet.num_dw * sizeof(u32), false, true);
        Vulkan::SrtNoteLanding(packet.Address<VAddr>(), packet.num_dw * sizeof(u32));
        break;
    default:
        UNREACHABLE();
    }
    switch (packet.int_sel.Value()) {
    case InterruptSelect::None:
        break;
    case InterruptSelect::IrqUndocumented:
    case InterruptSelect::IrqWhenWriteConfirm:
        Platform::IrqC::Instance()->Signal(static_cast<Platform::InterruptId>(task.pipe_id));
        break;
    default:
        UNREACHABLE();
    }
}

void RunReleaseMem(Vulkan::Rasterizer& rasterizer, const u8* data) {
    const auto task = *reinterpret_cast<const ReleaseMemTask*>(data);
    rasterizer.OnFence();
    // A GDS store copies on this thread; plain labels are written after the host copies
    // queued before them, without waiting here (as RunEventWriteEop).
    if (task.packet.data_sel.Value() != DataSelect::GdsMemStore &&
        Vulkan::DrawPipe::OnStageB() && !Gow3Toggle::Disabled(Gow3Toggle::RecorderFences)) {
        rasterizer.SignalAfterHostCopies(
            [task, &rasterizer] { SignalReleaseMem(rasterizer, task); });
    } else {
        rasterizer.WaitHostCopies();
        rasterizer.WaitDeferredSignals();
        SignalReleaseMem(rasterizer, task);
    }
}

// uc3 (GOW3_B_CENSUS): names for the stage B task census (registered always: the flag may not
// be read yet during static initialization).
[[maybe_unused]] const bool task_names_registered = [] {
    {
        Gow3Stats::NameTask(reinterpret_cast<const void*>(&RunEventWriteEop), "EOP");
        Gow3Stats::NameTask(reinterpret_cast<const void*>(&RunEventWriteEos), "EOS");
        Gow3Stats::NameTask(reinterpret_cast<const void*>(&RunReleaseMem), "ReleaseMem");
        Gow3Stats::NameTask(reinterpret_cast<const void*>(&RunWriteData), "WriteData");
        Gow3Stats::NameTask(reinterpret_cast<const void*>(&RunDmaData), "DmaData");
        Gow3Stats::NameTask(reinterpret_cast<const void*>(&SignalFlip), "flip");
        Gow3Stats::NameTask(reinterpret_cast<const void*>(&RunCommandTask), "readback command");
    }
    return true;
}();
} // namespace

void Liverpool::NotePendingFences(const auto& event) {
    const u64 position = rasterizer->DrawPipeHead();
    while (!pending_fences.empty() && rasterizer->DrawPipeReached(pending_fences.front().position)) {
        pending_fences.pop_front();
    }
    constexpr bool is_eop = requires { event.SignalFence([](void*, u64, u32) {}, [] {}); };
    const auto note = [&](void* address, u64 data, u32 num_bytes) {
        Gow3WriteLog::NoteIntent(reinterpret_cast<u64>(address), &data, num_bytes,
                               is_eop ? Gow3WriteLog::FenceIntent : Gow3WriteLog::EosIntent);
        pending_fences.push_back({reinterpret_cast<VAddr>(address), data, num_bytes, position});
        rasterizer->NotePendingGpuWrite(reinterpret_cast<VAddr>(address), num_bytes);
    };
    if constexpr (requires { event.SignalFence(note, [] {}); }) {
        event.SignalFence(note, [] {});
    } else {
        event.SignalFence(note);
    }
}

void Liverpool::NotePendingWrite(const PM4CmdWriteData& write_data, u32 num_bytes) {
    if (num_bytes > sizeof(u64)) {
        return; // labels are small; larger writes are not waited on
    }
    u64 data = 0;
    std::memcpy(&data, write_data.data, num_bytes);
    pending_fences.push_back(
        {write_data.Address<VAddr>(), data, num_bytes, rasterizer->DrawPipeHead()});
}

void Liverpool::NotePendingReleaseMem(const PM4CmdReleaseMem& release_mem) {
    u64 data = 0;
    u32 num_bytes = 0;
    switch (release_mem.data_sel.Value()) {
    case DataSelect::Data32Low:
        data = release_mem.DataDWord();
        num_bytes = sizeof(u32);
        break;
    case DataSelect::Data64:
        data = release_mem.DataQWord();
        num_bytes = sizeof(u64);
        break;
    default:
        return; // clocks and GDS stores are not labels
    }
    const u64 position = rasterizer->DrawPipeHead();
    while (!pending_fences.empty() && rasterizer->DrawPipeReached(pending_fences.front().position)) {
        pending_fences.pop_front();
    }
    const VAddr address = release_mem.Address<VAddr>();
    pending_fences.push_back({address, data, num_bytes, position});
    rasterizer->NotePendingGpuWrite(address, num_bytes);
}

bool Liverpool::PendingFenceValue(VAddr address, u32& value) {
    for (auto it = pending_fences.rbegin(); it != pending_fences.rend(); ++it) {
        if (rasterizer->DrawPipeReached(it->position)) {
            break; // written already: memory holds it (or a later write)
        }
        if (address >= it->address && address + sizeof(u32) <= it->address + it->num_bytes) {
            u64 data = it->data;
            value = static_cast<u32>(data >> ((address - it->address) * 8));
            return true;
        }
    }
    return false;
}

void Liverpool::ReportStuckWait(const PM4CmdWaitRegMem& wait, const char* queue) {
    const bool memory = wait.mem_space.Value() == PM4CmdWaitRegMem::MemSpace::Memory;
    const u32 current = memory ? *wait.Address<const u32*>() : regs.reg_array[wait.Reg()];
    u32 pending = 0;
    const bool has_pending = memory && rasterizer &&
                             PendingFenceValue(wait.Address<VAddr>(), pending);
    std::printf("uc3: %s WaitRegMem stuck 2 s: %s 0x%llx function %u ref 0x%x mask 0x%x, "
                "now 0x%x, pending fence %s0x%x; %u submits queued (gfx %zu)",
                queue, memory ? "memory" : "register",
                memory ? static_cast<unsigned long long>(wait.Address<VAddr>())
                       : static_cast<unsigned long long>(wait.Reg()),
                static_cast<unsigned>(wait.function.Value()), wait.ref, wait.mask, current,
                has_pending ? "" : "none ", pending, num_submits.load(),
                mapped_queues[GfxQueueId].submits.size());
    if (rasterizer) {
        auto& scheduler = rasterizer->GetScheduler();
        const auto [issued, done] = scheduler.DeferredSignalCounts();
        const u64 tick = scheduler.CurrentTick();
        std::printf("; draw pipe %s, tick %llu (previous %s), deferred signals %llu issued "
                    "%llu done, %zu chunks queued",
                    rasterizer->DrawPipeIdle() ? "idle" : "busy",
                    static_cast<unsigned long long>(tick),
                    tick > 1 && scheduler.IsFree(tick - 1) ? "done" : "not done",
                    static_cast<unsigned long long>(issued), static_cast<unsigned long long>(done),
                    scheduler.QueuedChunks());
    }
    std::printf("\n");
    std::fflush(stdout);
}

Liverpool::Task Liverpool::ProcessCeUpdate(std::span<const u32> ccb) {
    FIBER_ENTER(ccb_task_name);

    while (!ccb.empty()) {
        ProcessCommands();

        const auto* header = reinterpret_cast<const PM4Header*>(ccb.data());
        const u32 type = header->type;
        if (type != 3) {
            // No other types of packets were spotted so far
            UNREACHABLE_MSG("Invalid PM4 type {}", type);
        }

        const PM4ItOpcode opcode = header->type3.opcode;
        const auto* it_body = reinterpret_cast<const u32*>(header) + 1;
        switch (opcode) {
        case PM4ItOpcode::Nop: {
            // const auto* nop = reinterpret_cast<const PM4CmdNop*>(header);
            break;
        }
        case PM4ItOpcode::WriteConstRam: {
            const auto* write_const = reinterpret_cast<const PM4WriteConstRam*>(header);
            memcpy(cblock.constants_heap.data() + write_const->Offset(), &write_const->data,
                   write_const->Size());
            break;
        }
        case PM4ItOpcode::DumpConstRam: {
            // gow3: earlier draws on the recording thread may still read constants here.
            if (rasterizer) {
                rasterizer->DrainDrawPipe(Vulkan::DrawPipe::ReasonConstRam);
                rasterizer->WaitRingCopies();
            }
            const auto* dump_const = reinterpret_cast<const PM4DumpConstRam*>(header);
            if (rasterizer) {
                rasterizer->NoteGuestWrite(dump_const->Address<VAddr>(), dump_const->Size());
            }
            memcpy(dump_const->Address<void*>(),
                   cblock.constants_heap.data() + dump_const->Offset(), dump_const->Size());
            // uc3 R.21: stamped again once written, for walks that read before the copy.
            Vulkan::SrtNoteWrite(dump_const->Address<VAddr>(), dump_const->Size(),
                                 Vulkan::SrtOriginRewrite);
            break;
        }
        case PM4ItOpcode::IncrementCeCounter: {
            ++cblock.ce_count;
            break;
        }
        case PM4ItOpcode::WaitOnDeCounterDiff: {
            const auto diff = it_body[0];
            while ((cblock.de_count - cblock.ce_count) >= diff) {
                YIELD_CE();
            }
            break;
        }
        case PM4ItOpcode::IndirectBufferConst: {
            const auto* indirect_buffer = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
            auto task =
                ProcessCeUpdate({indirect_buffer->Address<const u32>(), indirect_buffer->ib_size});
            RESUME_CE(task);

            while (!task.handle.done()) {
                YIELD_CE();
                RESUME_CE(task);
            }
            task.handle.destroy();
            break;
        }
        default:
            const u32 count = header->type3.NumWords();
            UNREACHABLE_MSG("Unknown PM4 type 3 opcode {:#x} with count {}",
                            static_cast<u32>(opcode), count);
        }
        ccb = NextPacket(ccb, header->type3.NumWords() + 1);
    }

    FIBER_EXIT;
}

// gow3: graphics-register effects of a type-3 packet. The GPU thread and the draw-preparation
// workers both go through this function, and both fold every register-writing packet into a
// running checksum: equal checksums at a draw mean equal register files.
u64 Liverpool::HashRegisterPacket(u64 checksum, const u32* words, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        checksum = (checksum ^ words[i]) * 0x9E3779B97F4A7C15ull;
        checksum ^= checksum >> 29;
    }
    return checksum;
}

void Liverpool::ApplyGraphicsRegisterPacket(Regs& regs, const PM4Header* header, u64& checksum,
                                             RegDirty* dirty) {
    const u32 count = header->type3.NumWords();
    const auto* payload = reinterpret_cast<const u32*>(header + 2);
    const auto mark = [&](u32 word, u32 words) {
        if (dirty) {
            dirty->Mark(word, words);
        }
    };
    const auto mark_field = [&](const auto& field) {
        if (dirty) {
            dirty->MarkField(regs, field);
        }
    };
    switch (header->type3.opcode) {
    case PM4ItOpcode::ClearState:
        regs.SetDefaults();
        if (dirty) {
            dirty->Clear();
            dirty->reset = true;
        }
        break;
    case PM4ItOpcode::SetConfigReg: {
        const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
        std::memcpy(&regs.reg_array[Regs::ConfigRegWordOffset + set_data->reg_offset], payload,
                    (count - 1) * sizeof(u32));
        mark(Regs::ConfigRegWordOffset + set_data->reg_offset, count - 1);
        break;
    }
    case PM4ItOpcode::SetContextReg: {
        const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
        std::memcpy(&regs.reg_array[Regs::ContextRegWordOffset + set_data->reg_offset], payload,
                    (count - 1) * sizeof(u32));
        mark(Regs::ContextRegWordOffset + set_data->reg_offset, count - 1);
        break;
    }
    case PM4ItOpcode::SetShReg: {
        const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
        // The compute program range goes to the queue's cs_state, not to regs.
        if (!(set_data->reg_offset >= 0x200 &&
              set_data->reg_offset <= (0x200 + sizeof(ComputeProgram) / 4))) {
            std::memcpy(&regs.reg_array[Regs::ShRegWordOffset + set_data->reg_offset], payload,
                        (count - 1) * sizeof(u32));
            mark(Regs::ShRegWordOffset + set_data->reg_offset, count - 1);
        }
        break;
    }
    case PM4ItOpcode::SetUconfigReg: {
        const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
        std::memcpy(&regs.reg_array[Regs::UconfigRegWordOffset + set_data->reg_offset], payload,
                    (count - 1) * sizeof(u32));
        mark(Regs::UconfigRegWordOffset + set_data->reg_offset, count - 1);
        break;
    }
    case PM4ItOpcode::IndexType:
        regs.index_buffer_type.raw = reinterpret_cast<const PM4CmdDrawIndexType*>(header)->raw;
        mark_field(regs.index_buffer_type);
        break;
    case PM4ItOpcode::DrawIndex2: {
        const auto* draw_index = reinterpret_cast<const PM4CmdDrawIndex2*>(header);
        regs.max_index_size = draw_index->max_size;
        regs.index_base_address.base_addr_lo = draw_index->index_base_lo;
        regs.index_base_address.base_addr_hi = draw_index->index_base_hi;
        regs.num_indices = draw_index->index_count;
        mark_field(regs.max_index_size);
        mark_field(regs.index_base_address);
        mark_field(regs.num_indices);
        regs.draw_initiator = draw_index->draw_initiator;
        mark_field(regs.draw_initiator);
        break;
    }
    case PM4ItOpcode::DrawIndexOffset2: {
        const auto* draw_index_off = reinterpret_cast<const PM4CmdDrawIndexOffset2*>(header);
        regs.max_index_size = draw_index_off->max_size;
        regs.num_indices = draw_index_off->index_count;
        regs.draw_initiator = draw_index_off->draw_initiator;
        mark_field(regs.max_index_size);
        mark_field(regs.num_indices);
        mark_field(regs.draw_initiator);
        break;
    }
    case PM4ItOpcode::DrawIndexAuto: {
        const auto* draw_index = reinterpret_cast<const PM4CmdDrawIndexAuto*>(header);
        mark_field(regs.num_indices);
        mark_field(regs.draw_initiator);
        regs.num_indices = draw_index->index_count;
        regs.draw_initiator = draw_index->draw_initiator;
        break;
    }
    case PM4ItOpcode::NumInstances:
        regs.num_instances.num_instances =
            reinterpret_cast<const PM4CmdDrawNumInstances*>(header)->num_instances;
        mark_field(regs.num_instances);
        break;
    case PM4ItOpcode::IndexBase: {
        const auto* index_base = reinterpret_cast<const PM4CmdDrawIndexBase*>(header);
        regs.index_base_address.base_addr_lo = index_base->addr_lo;
        regs.index_base_address.base_addr_hi = index_base->addr_hi;
        mark_field(regs.index_base_address);
        break;
    }
    case PM4ItOpcode::IndexBufferSize:
        regs.num_indices = reinterpret_cast<const PM4CmdDrawIndexBufferSize*>(header)->num_indices;
        mark_field(regs.num_indices);
        break;
    case PM4ItOpcode::EventWrite: {
        const auto* event = reinterpret_cast<const PM4CmdEventWrite*>(header);
        if (event->event_type.Value() != EventType::SoVgtStreamoutFlush) {
            return;
        }
        // TODO: handle proper synchronization, for now signal that update is done immediately
        regs.cp_strmout_cntl.offset_update_done = 1;
        mark_field(regs.cp_strmout_cntl);
        break;
    }
    default:
        return;
    }
    checksum = HashRegisterPacket(checksum, reinterpret_cast<const u32*>(header), count + 1);
}

// gow3: GOW3_DCB_STATS=1 — structure of graphics command buffers (read-only scan, printed every
// 5 s): how many buffers, draws per buffer, state set before the first draw. Input for
// processing command buffers on several threads.
namespace {
struct DcbStats {
    u64 buffers[2]{}, with_draws[2]{}, draws[2]{}, dispatches[2]{}, max_draws[2]{};
    u64 clear_first[2]{}, ctx_before[2]{}, sh_before[2]{}, ibs[2]{}, dwords[2]{};
    u64 hist[2][6]{}; // draws per buffer: 0, 1-9, 10-99, 100-499, 500-1999, 2000+
    std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
};
DcbStats g_dcb_stats;
int g_dcb_depth;

bool IsDrawOpcode(PM4ItOpcode op) {
    switch (op) {
    case PM4ItOpcode::DrawIndex2:
    case PM4ItOpcode::DrawIndexOffset2:
    case PM4ItOpcode::DrawIndexAuto:
    case PM4ItOpcode::DrawIndirect:
    case PM4ItOpcode::DrawIndirectMulti:
    case PM4ItOpcode::DrawIndexIndirect:
    case PM4ItOpcode::DrawIndexIndirectMulti:
    case PM4ItOpcode::DrawIndexIndirectCountMulti:
        return true;
    default:
        return false;
    }
}

void ScanDcb(std::span<const u32> dcb, int depth) {
    auto& st = g_dcb_stats;
    const int d = depth > 0 ? 1 : 0;
    u64 draws = 0, dispatches = 0, ctx = 0, sh = 0, ibs = 0;
    bool clear_first = false;
    st.dwords[d] += dcb.size();
    for (size_t at = 0; at < dcb.size();) {
        const auto* header = reinterpret_cast<const PM4Header*>(dcb.data() + at);
        if (header->type == 2) {
            ++at;
            continue;
        }
        if (header->type != 3) {
            break;
        }
        const PM4ItOpcode op = header->type3.opcode;
        if (IsDrawOpcode(op)) {
            ++draws;
        } else if (op == PM4ItOpcode::DispatchDirect || op == PM4ItOpcode::DispatchIndirect) {
            ++dispatches;
        } else if (op == PM4ItOpcode::IndirectBuffer) {
            ++ibs;
        } else if (draws == 0) {
            if (op == PM4ItOpcode::ClearState) {
                clear_first = true;
            } else if (op == PM4ItOpcode::SetContextReg) {
                ctx += header->type3.NumWords() - 1;
            } else if (op == PM4ItOpcode::SetShReg) {
                sh += header->type3.NumWords() - 1;
            }
        }
        at += header->type3.NumWords() + 1;
    }
    ++st.buffers[d];
    st.draws[d] += draws;
    st.dispatches[d] += dispatches;
    st.ibs[d] += ibs;
    st.max_draws[d] = std::max(st.max_draws[d], draws);
    if (draws) {
        ++st.with_draws[d];
        st.clear_first[d] += clear_first;
        st.ctx_before[d] += ctx;
        st.sh_before[d] += sh;
    }
    const int bucket = draws == 0 ? 0 : draws < 10 ? 1 : draws < 100 ? 2 : draws < 500 ? 3
                     : draws < 2000 ? 4 : 5;
    ++st.hist[d][bucket];

    const auto now = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(now - st.window).count();
    if (depth == 0 && secs >= 5.0) {
        for (int k = 0; k < 2; ++k) {
            const double with = std::max<u64>(st.with_draws[k], 1);
            std::printf("DCB stats %s: %.0f buffers/s (%.0f with draws), %.0f draws/s, %.0f "
                        "dispatches/s, max %llu draws in one buffer, %.0f IBs/s, %.0f kdw/s; "
                        "draw buffers: %.0f%% start with ClearState, %.0f ctx + %.0f sh reg "
                        "writes before first draw; draws/buffer hist 0:%llu 1-9:%llu 10-99:%llu "
                        "100-499:%llu 500-1999:%llu 2000+:%llu\n",
                        k ? "indirect" : "submitted", st.buffers[k] / secs, st.with_draws[k] / secs,
                        st.draws[k] / secs, st.dispatches[k] / secs,
                        static_cast<unsigned long long>(st.max_draws[k]), st.ibs[k] / secs,
                        st.dwords[k] / secs / 1000.0, 100.0 * st.clear_first[k] / with,
                        st.ctx_before[k] / with, st.sh_before[k] / with,
                        static_cast<unsigned long long>(st.hist[k][0]),
                        static_cast<unsigned long long>(st.hist[k][1]),
                        static_cast<unsigned long long>(st.hist[k][2]),
                        static_cast<unsigned long long>(st.hist[k][3]),
                        static_cast<unsigned long long>(st.hist[k][4]),
                        static_cast<unsigned long long>(st.hist[k][5]));
        }
        st = DcbStats{};
    }
}
} // namespace

Liverpool::Task Liverpool::ProcessGraphics(std::span<const u32> dcb, std::span<const u32> ccb,
                                           u64 seq) {
    FIBER_ENTER(dcb_task_name);
    if (rasterizer) {
        rasterizer->RingDedupClear();
    }
    // Top-level buffers enqueued for the draw preparation workers (nested IBs are not).
    Vulkan::DrawPreparation* draw_prep =
        seq != NoSeq && rasterizer ? &rasterizer->GetDrawPreparation() : nullptr;
    if (draw_prep) {
        draw_prep->BeginSubmission(seq, regs, gfx_reg_checksum);
    }
    static const bool dcb_stats = EmulatorSettingsImpl::Flag("GOW3_DCB_STATS", false);
    const int dcb_depth = g_dcb_depth;
    if (dcb_stats) {
        ScanDcb(dcb, dcb_depth);
    }

    cblock.Reset();

    // TODO: potentially, ASCs also can depend on CE and in this case the
    // CE task should be moved into more global scope
    Task ce_task{};

    if (!ccb.empty()) {
        // In case of CCB provided kick off CE asap to have the constant heap ready to use
        ce_task = ProcessCeUpdate(ccb);
        RESUME_GFX(ce_task);
    }

    const auto base_addr = reinterpret_cast<uintptr_t>(dcb.data());
    while (!dcb.empty()) {
        ProcessCommands();

        const auto* header = reinterpret_cast<const PM4Header*>(dcb.data());
        const u32 type = header->type;

        switch (type) {
        default:
            UNREACHABLE_MSG("Wrong PM4 type {}", type);
            break;
        case 0:
            UNREACHABLE_MSG("Unimplemented PM4 type 0, base reg: {}, size: {}",
                            header->type0.base.Value(), header->type0.NumWords());
            break;
        case 2:
            // Type-2 packet are used for padding purposes
            dcb = NextPacket(dcb, 1);
            continue;
        case 3:
            const u32 count = header->type3.NumWords();
            const PM4ItOpcode opcode = header->type3.opcode;
            ApplyGraphicsRegisterPacket(regs, header, gfx_reg_checksum, &pipe_dirty);
            // DmaData to 0x3022C does nothing here (skipped below): no need to wait.
            if (rasterizer && !PipelinedOpcode(opcode) &&
                !(opcode == PM4ItOpcode::DmaData &&
                  reinterpret_cast<const PM4DmaData*>(header)->dst_addr_lo == 0x3022C)) {
                rasterizer->DrainDrawPipe(static_cast<u32>(opcode));
                // They may write guest memory: not before fences deferred earlier.
                rasterizer->WaitDeferredSignals();
            }
            const u64 srt_seq_before = Vulkan::g_srt_write_seq.load(std::memory_order_relaxed);
            switch (opcode) {
            case PM4ItOpcode::Nop: {
                const auto* nop = reinterpret_cast<const PM4CmdNop*>(header);
                if (nop->header.count.Value() == 0) {
                    break;
                }

                switch (nop->data_block[0]) {
                case PM4CmdNop::PayloadType::PatchedFlip: {
                    // There is no evidence that GPU CP drives flip events by parsing
                    // special NOP packets. For convenience lets assume that it does.
                    // gow3: after the writes before it (the buffer label: WriteData, which
                    // may still be queued on the draw recording thread).
                    Gow3Stats::NoteFlipStage(1);
                    if (rasterizer) {
                        rasterizer->RunInOrder(&SignalFlip, header, sizeof(u32),
                                               Gow3Toggle::PipelinedMemoryWrites);
                    } else {
                        Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxFlip);
                    }
                    break;
                }
                case PM4CmdNop::PayloadType::DebugMarkerPush: {
                    if (guest_markers_enabled) {
                        const auto marker_sz = nop->header.count.Value() * 2;
                        const std::string_view label{
                            reinterpret_cast<const char*>(&nop->data_block[1]), marker_sz};
                        rasterizer->ScopeMarkerBegin(label, true);
                    }
                    break;
                }
                case PM4CmdNop::PayloadType::DebugColorMarkerPush: {
                    if (guest_markers_enabled) {
                        const auto marker_sz = nop->header.count.Value() * 2;
                        const std::string_view label{
                            reinterpret_cast<const char*>(&nop->data_block[1]), marker_sz};
                        const u32 color = *reinterpret_cast<const u32*>(
                            reinterpret_cast<const u8*>(&nop->data_block[1]) + marker_sz);
                        rasterizer->ScopedMarkerInsertColor(label, color, true);
                    }
                    break;
                }
                case PM4CmdNop::PayloadType::DebugMarkerPop: {
                    if (guest_markers_enabled) {
                        rasterizer->ScopeMarkerEnd(true);
                    }
                    break;
                }
                default:
                    break;
                }
                break;
            }
            case PM4ItOpcode::ContextControl: {
                break;
            }
            case PM4ItOpcode::ClearState:
            case PM4ItOpcode::SetConfigReg: {
                break; // registers: ApplyGraphicsRegisterPacket
            }
            case PM4ItOpcode::SetContextReg: {
                const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
                const auto reg_addr = Regs::ContextRegWordOffset + set_data->reg_offset;
                const auto* payload = reinterpret_cast<const u32*>(header + 2);

                // In the case of HW, render target memory has alignment as color block operates on
                // tiles. There is no information of actual resource extents stored in CB context
                // regs, so any deduction of it from slices/pitch will lead to a larger surface
                // created. The same applies to the depth targets. Fortunately, the guest always
                // sends a trailing NOP packet right after the context regs setup, so we can use the
                // heuristic below and extract the hint to determine actual resource dims.

                switch (reg_addr) {
                case ContextRegs::CbColor0Base:
                case ContextRegs::CbColor1Base:
                case ContextRegs::CbColor2Base:
                case ContextRegs::CbColor3Base:
                case ContextRegs::CbColor4Base:
                case ContextRegs::CbColor5Base:
                case ContextRegs::CbColor6Base:
                case ContextRegs::CbColor7Base: {
                    const auto col_buf_id = (reg_addr - ContextRegs::CbColor0Base) /
                                            (ContextRegs::CbColor1Base - ContextRegs::CbColor0Base);
                    ASSERT(col_buf_id < NUM_COLOR_BUFFERS);

                    const auto nop_offset = header->type3.count;
                    if (nop_offset == 0x0e || nop_offset == 0x0d || nop_offset == 0x0b) {
                        ASSERT_MSG(payload[nop_offset] == 0xc0001000,
                                   "NOP hint is missing in CB setup sequence");
                        last_cb_extent[col_buf_id].raw = payload[nop_offset + 1];
                    } else {
                        last_cb_extent[col_buf_id].raw = 0;
                    }
                    break;
                }
                case ContextRegs::CbColor0Cmask:
                case ContextRegs::CbColor1Cmask:
                case ContextRegs::CbColor2Cmask:
                case ContextRegs::CbColor3Cmask:
                case ContextRegs::CbColor4Cmask:
                case ContextRegs::CbColor5Cmask:
                case ContextRegs::CbColor6Cmask:
                case ContextRegs::CbColor7Cmask: {
                    const auto col_buf_id =
                        (reg_addr - ContextRegs::CbColor0Cmask) /
                        (ContextRegs::CbColor1Cmask - ContextRegs::CbColor0Cmask);
                    ASSERT(col_buf_id < NUM_COLOR_BUFFERS);

                    const auto nop_offset = header->type3.count;
                    if (nop_offset == 0x04) {
                        ASSERT_MSG(payload[nop_offset] == 0xc0001000,
                                   "NOP hint is missing in CB setup sequence");
                        last_cb_extent[col_buf_id].raw = payload[nop_offset + 1];
                    }
                    break;
                }
                case ContextRegs::DbZInfo: {
                    if (header->type3.count == 8) {
                        ASSERT_MSG(payload[20] == 0xc0001000,
                                   "NOP hint is missing in DB setup sequence");
                        last_db_extent.raw = payload[21];
                    } else {
                        last_db_extent.raw = 0;
                    }
                    break;
                }
                default:
                    break;
                }
                break;
            }
            case PM4ItOpcode::SetShReg: {
                const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
                const auto set_size = (count - 1) * sizeof(u32);

                if (set_data->reg_offset >= 0x200 &&
                    set_data->reg_offset <= (0x200 + sizeof(ComputeProgram) / 4)) {
                    ASSERT(set_size <= sizeof(ComputeProgram));
                    auto* addr = reinterpret_cast<u32*>(&mapped_queues[GfxQueueId].cs_state) +
                                 (set_data->reg_offset - 0x200);
                    std::memcpy(addr, header + 2, set_size);
                } // other SH registers: ApplyGraphicsRegisterPacket
                break;
            }
            case PM4ItOpcode::SetUconfigReg: {
                break; // registers: ApplyGraphicsRegisterPacket
            }
            case PM4ItOpcode::SetPredication: {
                // gow3: once; God of War III sets it around most draws (~1000 lines a frame).
                static std::once_flag predication_logged;
                std::call_once(predication_logged,
                               [] { LOG_WARNING(Render, "Unimplemented IT_SET_PREDICATION"); });
                break;
            }
            case PM4ItOpcode::IndexType: {
                break; // registers: ApplyGraphicsRegisterPacket
            }
            case PM4ItOpcode::DrawIndex2: {
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                const auto* prepared = draw_prep ? draw_prep->NextDraw() : nullptr;
                rasterizer->ScopeMarker("gfx:{}:DrawIndex2", fmt::make_format_args(cmd_address),
                                        [&] { rasterizer->Draw(true, 0, prepared); });
                break;
            }
            case PM4ItOpcode::DrawIndexOffset2: {
                const auto* draw_index_off =
                    reinterpret_cast<const PM4CmdDrawIndexOffset2*>(header);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                const auto* prepared = draw_prep ? draw_prep->NextDraw() : nullptr;
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndexOffset2", fmt::make_format_args(cmd_address),
                    [&] { rasterizer->Draw(true, draw_index_off->index_offset, prepared); });
                break;
            }
            case PM4ItOpcode::DrawIndexAuto: {
                const auto* draw_index = reinterpret_cast<const PM4CmdDrawIndexAuto*>(header);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                const auto* prepared = draw_prep ? draw_prep->NextDraw() : nullptr;
                rasterizer->ScopeMarker("gfx:{}:DrawIndexAuto", fmt::make_format_args(cmd_address),
                                        [&] { rasterizer->Draw(false, 0, prepared); });
                break;
            }
            case PM4ItOpcode::DrawIndirect: {
                const auto* draw_indirect = reinterpret_cast<const PM4CmdDrawIndirect*>(header);
                const auto offset = draw_indirect->data_offset;
                const auto stride = sizeof(DrawIndirectArgs);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndirect", fmt::make_format_args(cmd_address), [&] {
                        rasterizer->DrawIndirect(false, indirect_args_addr, offset, stride, 1, 0,
                                                 draw_indirect->base_vtx_loc,
                                                 draw_indirect->start_inst_loc);
                    });
                break;
            }
            case PM4ItOpcode::DrawIndirectCountMulti: {
                const auto* draw_indirect =
                    reinterpret_cast<const PM4CmdDrawIndirectCountMulti*>(header);
                const auto offset = draw_indirect->data_offset;
                const auto stride = sizeof(DrawIndirectArgs);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndirectCountMulti", fmt::make_format_args(cmd_address), [&] {
                        rasterizer->DrawIndirect(
                            false, indirect_args_addr, offset, draw_indirect->stride,
                            draw_indirect->count, draw_indirect->count_addr,
                            draw_indirect->base_vtx_loc, draw_indirect->start_inst_loc);
                    });
                break;
            }
            case PM4ItOpcode::DrawIndirectMulti: {
                const auto* draw_indirect =
                    reinterpret_cast<const PM4CmdDrawIndirectMulti*>(header);
                const auto offset = draw_indirect->data_offset;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndirectMulti", fmt::make_format_args(cmd_address), [&] {
                        rasterizer->DrawIndirect(false, indirect_args_addr, offset,
                                                 draw_indirect->stride, draw_indirect->count, 0,
                                                 draw_indirect->base_vtx_loc,
                                                 draw_indirect->start_inst_loc);
                    });
                break;
            }
            case PM4ItOpcode::DrawIndexIndirect: {
                const auto* draw_index_indirect =
                    reinterpret_cast<const PM4CmdDrawIndexIndirect*>(header);
                const auto offset = draw_index_indirect->data_offset;
                const auto stride = sizeof(DrawIndexedIndirectArgs);
                regs.max_index_size = index_buffer_num_indices;
                MarkIndirectIndexSize(regs, pipe_dirty); // ucndc: GOW3_INDIRECT_IB_FIX
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndexIndirect", fmt::make_format_args(cmd_address), [&] {
                        rasterizer->DrawIndirect(true, indirect_args_addr, offset, stride, 1, 0,
                                                 draw_index_indirect->base_vtx_loc,
                                                 draw_index_indirect->start_inst_loc);
                    });
                break;
            }
            case PM4ItOpcode::DrawIndexIndirectMulti: {
                const auto* draw_index_indirect =
                    reinterpret_cast<const PM4CmdDrawIndexIndirectMulti*>(header);
                const auto offset = draw_index_indirect->data_offset;
                regs.max_index_size = index_buffer_num_indices;
                MarkIndirectIndexSize(regs, pipe_dirty); // ucndc: GOW3_INDIRECT_IB_FIX
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndexIndirectMulti", fmt::make_format_args(cmd_address), [&] {
                        rasterizer->DrawIndirect(
                            true, indirect_args_addr, offset, draw_index_indirect->stride,
                            draw_index_indirect->count, 0, draw_index_indirect->base_vtx_loc,
                            draw_index_indirect->start_inst_loc);
                    });
                break;
            }
            case PM4ItOpcode::DrawIndexIndirectCountMulti: {
                const auto* draw_index_indirect =
                    reinterpret_cast<const PM4CmdDrawIndexIndirectCountMulti*>(header);
                const auto offset = draw_index_indirect->data_offset;
                regs.max_index_size = index_buffer_num_indices;
                MarkIndirectIndexSize(regs, pipe_dirty); // ucndc: GOW3_INDIRECT_IB_FIX
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndexIndirectCountMulti", fmt::make_format_args(cmd_address), [&] {
                        rasterizer->DrawIndirect(
                            true, indirect_args_addr, offset, draw_index_indirect->stride,
                            draw_index_indirect->count,
                            draw_index_indirect->count_indirect_enable.Value()
                                ? draw_index_indirect->count_addr
                                : 0,
                            draw_index_indirect->base_vtx_loc, draw_index_indirect->start_inst_loc);
                    });
                break;
            }
            case PM4ItOpcode::DispatchDirect: {
                const auto* dispatch_direct = reinterpret_cast<const PM4CmdDispatchDirect*>(header);
                auto& cs_program = GetCsRegs();
                cs_program.dim_x = dispatch_direct->dim_x;
                cs_program.dim_y = dispatch_direct->dim_y;
                cs_program.dim_z = dispatch_direct->dim_z;
                cs_program.dispatch_initiator = dispatch_direct->dispatch_initiator;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                                   cs_program);
                }
                if (!rasterizer || (cs_program.dispatch_initiator & 1) == 0) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker("gfx:{}:DispatchDirect", fmt::make_format_args(cmd_address),
                                        [&] { rasterizer->DispatchDirect(); });
                break;
            }
            case PM4ItOpcode::DispatchIndirect: {
                const auto* dispatch_indirect =
                    reinterpret_cast<const PM4CmdDispatchIndirect*>(header);
                auto& cs_program = GetCsRegs();
                const auto offset = dispatch_indirect->data_offset;
                const auto size = sizeof(PM4CmdDispatchIndirect::GroupDimensions);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                                   cs_program);
                }
                if (!rasterizer || (cs_program.dispatch_initiator & 1) == 0) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DispatchIndirect", fmt::make_format_args(cmd_address),
                    [&] { rasterizer->DispatchIndirect(indirect_args_addr, offset, size); });
                break;
            }
            case PM4ItOpcode::NumInstances:
            case PM4ItOpcode::IndexBase:
            case PM4ItOpcode::IndexBufferSize: {
                if (opcode == PM4ItOpcode::IndexBufferSize) {
                    // uc3: the index buffer capacity for indirect draws (upstream #5278); the
                    // registers themselves come from ApplyGraphicsRegisterPacket.
                    index_buffer_num_indices =
                        reinterpret_cast<const PM4CmdDrawIndexBufferSize*>(header)->num_indices;
                }
                break; // registers: ApplyGraphicsRegisterPacket
            }
            case PM4ItOpcode::SetBase: {
                const auto* set_base = reinterpret_cast<const PM4CmdSetBase*>(header);
                ASSERT(set_base->base_index == PM4CmdSetBase::BaseIndex::DrawIndexIndirPatchTable);
                indirect_args_addr = set_base->Address<u64>();
                break;
            }
            case PM4ItOpcode::EventWrite: {
                const auto* event = reinterpret_cast<const PM4CmdEventWrite*>(header);
                LOG_DEBUG(Render, "Encountered EventWrite: event_type = {}, event_index = {}",
                          magic_enum::enum_name(event->event_type.Value()),
                          magic_enum::enum_name(event->event_index.Value()));
                if (event->event_type.Value() == EventType::SoVgtStreamoutFlush) {
                    // registers: ApplyGraphicsRegisterPacket
                } else if (event->event_index.Value() == EventIndex::ZpassDone) {
                    if (event->event_type.Value() == EventType::PixelPipeStatDump) {
                        static constexpr u64 OcclusionCounterValidMask = 0x8000000000000000ULL;
                        // uc1 (white crates, diagnosis): GOW3_OCCLUSION_STEP=<n> replaces the step of
                        // the fake pixel counter (0: every query reads "nothing drawn").
                        static const u64 OcclusionCounterStep = [] {
                            const char* env = std::getenv("GOW3_OCCLUSION_STEP");
                            return env && env[0] ? u64(std::strtoull(env, nullptr, 0)) : 0x2FFFFFFULL;
                        }();
                        u64* results = event->Address<u64*>();
                        for (s32 i = 0; i < num_counter_pairs; ++i, results += 2) {
                            *results = pixel_counter | OcclusionCounterValidMask;
                        }
                        pixel_counter += OcclusionCounterStep;
                    }
                }
                break;
            }
            case PM4ItOpcode::EventWriteEos: {
                const auto* event_eos = reinterpret_cast<const PM4CmdEventWriteEos*>(header);
                if (rasterizer &&
                    rasterizer->RunInOrder(&RunEventWriteEos, event_eos, sizeof(*event_eos))) {
                    NotePendingFences(*event_eos);
                }
                break;
            }
            case PM4ItOpcode::EventWriteEop: {
                const auto* event_eop = reinterpret_cast<const PM4CmdEventWriteEop*>(header);
                if (rasterizer) {
                    if (rasterizer->RunInOrder(&RunEventWriteEop, event_eop, sizeof(*event_eop))) {
                        NotePendingFences(*event_eop);
                    }
                } else {
                    SignalEop(*event_eop);
                }
                break;
            }
            case PM4ItOpcode::DmaData: {
                const auto* dma_data = reinterpret_cast<const PM4DmaData*>(header);
                if (dma_data->dst_addr_lo == 0x3022C || !rasterizer) {
                    break;
                }
                if (rasterizer->RunInOrder(&RunDmaData, dma_data, sizeof(PM4DmaData),
                                           Gow3Toggle::PipelinedMemoryWrites) &&
                    (dma_data->dst_sel == DmaDataDst::Memory ||
                     dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                    rasterizer->NotePendingGpuWrite(dma_data->DstAddress<VAddr>(),
                                                    dma_data->NumBytes());
                }
                break;
            }
            case PM4ItOpcode::WriteData: {
                const auto* write_data = reinterpret_cast<const PM4CmdWriteData*>(header);
                ASSERT(write_data->dst_sel.Value() == 2 || write_data->dst_sel.Value() == 5);
                ASSERT(!write_data->wr_one_addr.Value());
                if (rasterizer) {
                    // In order with the draws (on the draw recording thread when in use).
                    Gow3WriteLog::NoteIntent(write_data->Address<u64>(), write_data->data,
                                           (count - 2) * sizeof(u32),
                                           Gow3WriteLog::WriteDataIntent);
                    if (rasterizer->RunInOrder(&RunWriteData, header, (count + 1) * sizeof(u32),
                                               Gow3Toggle::PipelinedMemoryWrites)) {
                        NotePendingWrite(*write_data, (count - 2) * sizeof(u32));
                        rasterizer->NotePendingGpuWrite(write_data->Address<VAddr>(),
                                                        (count - 2) * sizeof(u32));
                    }
                } else {
                    std::memcpy(write_data->Address<u64*>(), write_data->data, (count - 2) * 4);
                }
                break;
            }
            case PM4ItOpcode::CopyData: {
                const auto* copy_data = reinterpret_cast<const PM4CmdCopyData*>(header);
                LOG_WARNING(Render,
                            "unhandled IT_COPY_DATA src_sel = {}, dst_sel = {}, "
                            "count_sel = {}, wr_confirm = {}, engine_sel = {}",
                            u32(copy_data->src_sel.Value()), u32(copy_data->dst_sel.Value()),
                            copy_data->count_sel.Value(), copy_data->wr_confirm.Value(),
                            u32(copy_data->engine_sel.Value()));
                break;
            }
            case PM4ItOpcode::MemSemaphore: {
                const auto* mem_semaphore = reinterpret_cast<const PM4CmdMemSemaphore*>(header);
                if (rasterizer) {
                    rasterizer->RingDedupClear();
                }
                if (mem_semaphore->IsSignaling()) {
                    // R.9.12: packets that write guest memory on this thread (not drained,
                    // not handed to the recording thread) wait for queued ring copies first.
                    if (rasterizer) {
                        rasterizer->WaitRingCopies();
                    }
                    mem_semaphore->Signal();
                } else {
                    while (!mem_semaphore->Signaled()) {
                        YIELD_GFX();
                    }
                    mem_semaphore->Decrement();
                }
                break;
            }
            case PM4ItOpcode::AcquireMem: {
                // const auto* acquire_mem = reinterpret_cast<PM4CmdAcquireMem*>(header);
                break;
            }
            case PM4ItOpcode::Rewind: {
                if (!rasterizer) {
                    break;
                }
                rasterizer->RingDedupClear();
                const PM4CmdRewind* rewind = reinterpret_cast<const PM4CmdRewind*>(header);
                while (!rewind->Valid()) {
                    YIELD_GFX();
                }
                break;
            }
            case PM4ItOpcode::WaitRegMem: {
                const auto* wait_reg_mem = reinterpret_cast<const PM4CmdWaitRegMem*>(header);
                if (rasterizer) {
                    rasterizer->RingDedupClear();
                }
                // ASSERT(wait_reg_mem->engine.Value() == PM4CmdWaitRegMem::Engine::Me);
                // Optimization: VO label waits are special because the emulator
                // will write to the label when presentation is finished. So if
                // there are no other submits to yield to we can sleep the thread
                // instead and allow other tasks to run.
                const u64* wait_addr = wait_reg_mem->Address<u64*>();
                if (vo_port->IsVoLabel(wait_addr) &&
                    num_submits == mapped_queues[GfxQueueId].submits.size()) {
                    // uc3 T3b (GOW3_VO_SKIP_DRAIN=1): a label already free needs no drain.
                    static const bool skip_drain = [] {
                        const char* env = std::getenv("GOW3_VO_SKIP_DRAIN");
                        return env && env[0] == '1';
                    }();
                    static std::atomic<u64> vo_waits{0}, vo_free{0};
                    ++vo_waits;
                    if (wait_reg_mem->Test(regs.reg_array)) {
                        ++vo_free;
                        if (skip_drain) {
                            break;
                        }
                    }
                    if (std::getenv("GOW3_FRAME_STATS") && (vo_waits & 255) == 0) {
                        std::printf("VO label waits: %llu, %llu already free\n",
                                    (unsigned long long)vo_waits.load(),
                                    (unsigned long long)vo_free.load());
                    }
                    if (rasterizer) {
                        rasterizer->DrainDrawPipe(static_cast<u32>(opcode));
                    }
                    vo_port->WaitVoLabel([&] { return wait_reg_mem->Test(regs.reg_array); });
                    break;
                }
                // gow3: a fence handed to the draw recording thread counts as written: that
                // thread runs everything in stream order.
                if (u32 value; wait_reg_mem->mem_space.Value() ==
                                   PM4CmdWaitRegMem::MemSpace::Memory &&
                               !Gow3Toggle::Disabled(Gow3Toggle::PendingFenceWaits) &&
                               PendingFenceValue(wait_reg_mem->Address<VAddr>(), value) &&
                               wait_reg_mem->TestValue(value)) {
                    break;
                }
                // Else the value may come from a fence that thread has yet to write: let it
                // catch up before yielding to other queues.
                // uc3: a wait unmet for 2 s is a hang in the making; say what it waits for.
                const auto wait_started = std::chrono::steady_clock::now();
                bool wait_reported = false;
                while (!wait_reg_mem->Test(regs.reg_array)) {
                    if (rasterizer && !rasterizer->DrawPipeIdle()) {
                        rasterizer->DrainDrawPipe(static_cast<u32>(opcode));
                        continue;
                    }
                    if (!wait_reported &&
                        std::chrono::steady_clock::now() - wait_started > std::chrono::seconds{2}) {
                        wait_reported = true;
                        ReportStuckWait(*wait_reg_mem, "gfx");
                    }
                    YIELD_GFX();
                }
                break;
            }
            case PM4ItOpcode::IndirectBuffer: {
                const auto* indirect_buffer = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
                auto task = ProcessGraphics(
                    {indirect_buffer->Address<const u32>(), indirect_buffer->ib_size}, {});
                // Tasks start suspended: the nested body (and its scan) runs on this resume.
                g_dcb_depth = dcb_depth + 1;
                RESUME_GFX(task);
                g_dcb_depth = dcb_depth;

                while (!task.handle.done()) {
                    YIELD_GFX();
                    RESUME_GFX(task);
                }
                task.handle.destroy();
                break;
            }
            case PM4ItOpcode::IncrementDeCounter: {
                ++cblock.de_count;
                break;
            }
            case PM4ItOpcode::WaitOnCeCounter: {
                if (rasterizer) {
                    rasterizer->RingDedupClear();
                }
                while (cblock.ce_count <= cblock.de_count && !ce_task.handle.done()) {
                    RESUME_GFX(ce_task);
                }
                break;
            }
            case PM4ItOpcode::PfpSyncMe: {
                break;
            }
            case PM4ItOpcode::StrmoutBufferUpdate: {
                const auto* strmout = reinterpret_cast<const PM4CmdStrmoutBufferUpdate*>(header);
                LOG_WARNING(Render_Vulkan,
                            "Unimplemented IT_STRMOUT_BUFFER_UPDATE, update_memory = {}, "
                            "source_select = {}, buffer_select = {}",
                            strmout->update_memory.Value(),
                            magic_enum::enum_name(strmout->source_select.Value()),
                            strmout->buffer_select.Value());
                break;
            }
            case PM4ItOpcode::GetLodStats: {
                LOG_WARNING(Render_Vulkan, "Unimplemented IT_GET_LOD_STATS");
                break;
            }
            case PM4ItOpcode::CondExec: {
                const auto* cond_exec = reinterpret_cast<const PM4CmdCondExec*>(header);
                if (cond_exec->command.Value() != 0) {
                    LOG_WARNING(Render, "IT_COND_EXEC used a reserved command");
                }
                const auto skip = *cond_exec->Address() == false;
                if (skip) {
                    dcb = NextPacket(dcb,
                                     header->type3.NumWords() + 1 + cond_exec->exec_count.Value());
                    continue;
                }
                break;
            }
            default:
                UNREACHABLE_MSG("Unknown PM4 type 3 opcode {:#x} with count {}",
                                static_cast<u32>(opcode), count);
            }
            // uc3 R.21: a wait may release data the CPU wrote while the GPU waited; a packet
            // that wrote guest memory without noting its range counts as a global change.
            // R.10 A1: graphics waits are evaluated by the prep worker instead, and a packet's
            // own range is stamped when known.
            if (opcode == PM4ItOpcode::MemSemaphore || opcode == PM4ItOpcode::Rewind ||
                opcode == PM4ItOpcode::WaitRegMem || opcode == PM4ItOpcode::WaitOnCeCounter) {
                if (!Vulkan::SrtA1()) {
                    Vulkan::SrtGlobalEvent(0);
                }
            } else if ((opcode == PM4ItOpcode::DmaData || opcode == PM4ItOpcode::WriteData ||
                        opcode == PM4ItOpcode::CopyData || opcode == PM4ItOpcode::EventWriteEop ||
                        opcode == PM4ItOpcode::EventWriteEos ||
                        opcode == PM4ItOpcode::ReleaseMem) &&
                       Vulkan::g_srt_write_seq.load(std::memory_order_relaxed) == srt_seq_before &&
                       !(Vulkan::SrtA1() && SrtNotePacketRange(&header->type3))) {
                Vulkan::SrtGlobalEvent(3);
            }
            dcb = NextPacket(dcb, header->type3.NumWords() + 1);
            break;
        }
    }

    if (ce_task.handle) {
        while (!ce_task.handle.done()) {
            RESUME_GFX(ce_task);
        }
        ce_task.handle.destroy();
    }

    if (rasterizer && seq != NoSeq) {
        rasterizer->RetireSubmission(); // prepared draws live until the recording thread is past
    }
    if (draw_prep) {
        draw_prep->EndSubmission();
    }
    if (seq != NoSeq && Gow3Stats::enabled) {
        Gow3Stats::submissions.fetch_add(1, std::memory_order_relaxed);
#ifndef _WIN32 // gow3: per-thread rusage statistics are Linux-only
        if (rusage usage{}; getrusage(RUSAGE_THREAD, &usage) == 0) {
            Gow3Stats::gpu_user_us.store(u64(usage.ru_utime.tv_sec) * 1000000 + usage.ru_utime.tv_usec,
                                       std::memory_order_relaxed);
            Gow3Stats::gpu_sys_us.store(u64(usage.ru_stime.tv_sec) * 1000000 + usage.ru_stime.tv_usec,
                                      std::memory_order_relaxed);
            Gow3Stats::gpu_invol_switches.store(usage.ru_nivcsw, std::memory_order_relaxed);
            Gow3Stats::gpu_vol_switches.store(usage.ru_nvcsw, std::memory_order_relaxed);
            Gow3Stats::gpu_minor_faults.store(usage.ru_minflt, std::memory_order_relaxed);
        }
#endif
    }

    FIBER_EXIT;
}

template <bool is_indirect>
Liverpool::Task Liverpool::ProcessCompute(std::span<const u32> acb, u32 vqid) {
    FIBER_ENTER(acb_task_name[vqid]);
    if (rasterizer) {
        rasterizer->RingDedupClear();
    }
    auto& queue = asc_queues[{vqid}];

    struct IndirectPatch {
        const PM4Header* header;
        VAddr indirect_addr;
    };
    boost::container::small_vector<IndirectPatch, 4> indirect_patches;

    auto base_addr = reinterpret_cast<VAddr>(acb.data());
    size_t acb_size = acb.size_bytes();
    while (!acb.empty()) {
        ProcessCommands();
        auto* header = reinterpret_cast<const PM4Header*>(acb.data());
        // uc3 (GOW3_COMPUTE_DRAIN_LAZY=1, off by default): with GOW3_COMPUTE_PIPE off, packets
        // that only set registers or chain buffers skip the wait for the recording thread; the
        // dispatches and memory packets after them still wait. UC2 rooftop: ~55k waits/s here.
        // uc3 (GOW3_HLE_INORDER=1, off by default): register packets, dispatches and fences
        // skip it too; DispatchDirect drains unless the dispatch is a scalar copy it runs in
        // order, and ReleaseMem runs in order (RunReleaseMem).
        const bool inorder_skip = rasterizer && !ComputePipeOn() && Vulkan::HleInOrderEnabled() &&
                                  queue.tmp_dwords == 0 && header->type == 3 &&
                                  (ComputeRegisterOpcode(header->type3.opcode) ||
                                   header->type3.opcode == PM4ItOpcode::DispatchDirect ||
                                   header->type3.opcode == PM4ItOpcode::ReleaseMem);
        if (rasterizer && !ComputePipeOn() && !inorder_skip &&
            !(ComputeDrainLazy() && queue.tmp_dwords == 0 && header->type == 3 &&
              ComputeRegisterOpcode(header->type3.opcode))) {
            rasterizer->DrainDrawPipe(Vulkan::DrawPipe::ReasonCompute); // gow3: compute work uses the caches
            if (Vulkan::HleInOrderEnabled()) {
                // uc3: which packets still drain with GOW3_HLE_INORDER.
                static std::array<std::atomic<u32>, 256> drained_by{};
                static std::atomic<u32> drained{0};
                const u32 op = header->type == 3 ? static_cast<u32>(header->type3.opcode.Value()) & 0xff
                                                 : 0;
                drained_by[op].fetch_add(1, std::memory_order_relaxed);
                if (((drained.fetch_add(1, std::memory_order_relaxed) + 1) & 131071) == 0) {
                    std::string top;
                    for (u32 i = 0; i < 256; ++i) {
                        const u32 c = drained_by[i].load(std::memory_order_relaxed);
                        if (c > 1000) {
                            top += fmt::format(" {:#x}={}", i, c);
                        }
                    }
                    std::printf("uc3: compute drains by opcode (type %u last, tmp %u):%s\n",
                                u32(header->type), queue.tmp_dwords, top.c_str());
                }
            }
        }
        const bool acb_undrained = inorder_skip && header->type3.opcode == PM4ItOpcode::DispatchDirect;

        u32 next_dw_off = header->type3.NumWords() + 1;

        // If we have a buffered packet, use it.
        if (queue.tmp_dwords > 0) [[unlikely]] {
            header = reinterpret_cast<const PM4Header*>(queue.tmp_packet.data());
            next_dw_off = header->type3.NumWords() + 1 - queue.tmp_dwords;
            std::memcpy(queue.tmp_packet.data() + queue.tmp_dwords, acb.data(),
                        next_dw_off * sizeof(u32));
            queue.tmp_dwords = 0;
        }

        // If the packet is split across ring boundary, buffer until next submission
        if (next_dw_off > acb.size()) [[unlikely]] {
            std::memcpy(queue.tmp_packet.data(), acb.data(), acb.size_bytes());
            queue.tmp_dwords = acb.size();
            if constexpr (!is_indirect) {
                *queue.read_addr += acb.size();
                *queue.read_addr %= queue.ring_size_dw;
            }
            break;
        }

        if (header->type == 2) {
            // Type-2 packet are used for padding purposes
            next_dw_off = 1;
            acb = NextPacket(acb, next_dw_off);
            if constexpr (!is_indirect) {
                *queue.read_addr += next_dw_off;
                *queue.read_addr %= queue.ring_size_dw;
            }
            continue;
        }

        if (header->type != 3) {
            // No other types of packets were spotted so far
            UNREACHABLE_MSG("Invalid PM4 type {}", header->type.Value());
        }

        const PM4ItOpcode opcode = header->type3.opcode;
        // uc3 (Fable R.8.3, GOW3_COMPUTE_PIPE=1): like the DCB loop, wait for the recording
        // thread only before packets that touch the caches or guest memory synchronously.
        // Dispatches go through the pipe (or drain themselves when it is off).
        if (rasterizer && ComputePipeOn() && !ComputePipelinedOpcode(opcode)) {
            // A DmaData that patches the dims of a later DispatchDirect in this buffer only
            // records the patch (indirect_patches): no rasterizer work, no wait.
            bool dispatch_patch = false;
            if (opcode == PM4ItOpcode::DmaData) {
                const auto* dma = reinterpret_cast<const PM4DmaData*>(header);
                if (dma->dst_addr_lo == 0x3022C) {
                    dispatch_patch = true;
                } else if ((dma->src_sel == DmaDataSrc::Memory ||
                            dma->src_sel == DmaDataSrc::MemoryUsingL2) &&
                           (dma->dst_sel == DmaDataDst::Memory ||
                            dma->dst_sel == DmaDataDst::MemoryUsingL2)) {
                    const VAddr dst = dma->DstAddress<VAddr>();
                    const auto* target = reinterpret_cast<const PM4Header*>(dst - sizeof(PM4Header));
                    dispatch_patch = dst >= base_addr && dst < base_addr + acb_size &&
                                     dma->NumBytes() ==
                                         sizeof(PM4CmdDispatchIndirect::GroupDimensions) &&
                                     target->type == 3 &&
                                     target->type3.opcode == PM4ItOpcode::DispatchDirect;
                }
            }
            if (!dispatch_patch) {
                rasterizer->DrainDrawPipe(static_cast<u32>(opcode));
            }
        }

        const auto* it_body = reinterpret_cast<const u32*>(header) + 1;
        const u64 srt_seq_before = Vulkan::g_srt_write_seq.load(std::memory_order_relaxed);
        switch (opcode) {
        case PM4ItOpcode::Nop: {
            const auto* nop = reinterpret_cast<const PM4CmdNop*>(header);
            break;
        }
        case PM4ItOpcode::IndirectBuffer: {
            const auto* indirect_buffer = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
            auto task = ProcessCompute<true>(
                {indirect_buffer->Address<const u32>(), indirect_buffer->ib_size}, vqid);
            RESUME_ASC(task, vqid);

            while (!task.handle.done()) {
                YIELD_ASC(vqid);
                RESUME_ASC(task, vqid);
            }
            task.handle.destroy();
            break;
        }
        case PM4ItOpcode::DmaData: {
            const auto* dma_data = reinterpret_cast<const PM4DmaData*>(header);
            if (dma_data->dst_addr_lo == 0x3022C || !rasterizer) {
                break;
            }
            ASSERT(dma_data->command.das == 0);
            if (dma_data->src_sel == DmaDataSrc::Data && dma_data->dst_sel == DmaDataDst::Gds) {
                rasterizer->FillBuffer(dma_data->dst_addr_lo, dma_data->NumBytes(), dma_data->data,
                                       true);
            } else if ((dma_data->src_sel == DmaDataSrc::Memory ||
                        dma_data->src_sel == DmaDataSrc::MemoryUsingL2) &&
                       dma_data->dst_sel == DmaDataDst::Gds) {
                rasterizer->CopyBuffer(dma_data->dst_addr_lo, dma_data->SrcAddress<VAddr>(),
                                       dma_data->NumBytes(), true, false);
            } else if (dma_data->src_sel == DmaDataSrc::Data &&
                       (dma_data->dst_sel == DmaDataDst::Memory ||
                        dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                rasterizer->FillBuffer(dma_data->DstAddress<VAddr>(), dma_data->NumBytes(),
                                       dma_data->data, false);
            } else if (dma_data->src_sel == DmaDataSrc::Gds &&
                       (dma_data->dst_sel == DmaDataDst::Memory ||
                        dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                rasterizer->CopyBuffer(dma_data->DstAddress<VAddr>(), dma_data->src_addr_lo,
                                       dma_data->NumBytes(), false, true);
            } else if ((dma_data->src_sel == DmaDataSrc::Memory ||
                        dma_data->src_sel == DmaDataSrc::MemoryUsingL2) &&
                       (dma_data->dst_sel == DmaDataDst::Memory ||
                        dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                const u32 num_bytes = dma_data->NumBytes();
                const VAddr src_addr = dma_data->SrcAddress<VAddr>();
                const VAddr dst_addr = dma_data->DstAddress<VAddr>();
                const PM4Header* header =
                    reinterpret_cast<const PM4Header*>(dst_addr - sizeof(PM4Header));
                if (dst_addr >= base_addr && dst_addr < base_addr + acb_size &&
                    num_bytes == sizeof(PM4CmdDispatchIndirect::GroupDimensions) &&
                    header->type == 3 && header->type3.opcode == PM4ItOpcode::DispatchDirect) {
                    indirect_patches.emplace_back(header, src_addr);
                } else {
                    rasterizer->CopyBuffer(dst_addr, src_addr, num_bytes, false, false);
                }
            } else {
                UNREACHABLE_MSG("WriteData src_sel = {}, dst_sel = {}", u32(dma_data->src_sel),
                                u32(dma_data->dst_sel));
            }
            break;
        }
        case PM4ItOpcode::AcquireMem: {
            break;
        }
        case PM4ItOpcode::Rewind: {
            if (!rasterizer) {
                break;
            }
            rasterizer->RingDedupClear();
            const PM4CmdRewind* rewind = reinterpret_cast<const PM4CmdRewind*>(header);
            while (!rewind->Valid()) {
                YIELD_ASC(vqid);
            }
            break;
        }
        case PM4ItOpcode::SetShReg: {
            const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
            const auto set_size = (header->type3.NumWords() - 1) * sizeof(u32);

            if (set_data->reg_offset >= 0x200 &&
                set_data->reg_offset <= (0x200 + sizeof(ComputeProgram) / 4)) {
                ASSERT(set_size <= sizeof(ComputeProgram));
                auto* addr = reinterpret_cast<u32*>(&mapped_queues[vqid + 1].cs_state) +
                             (set_data->reg_offset - 0x200);
                std::memcpy(addr, header + 2, set_size);
            } else {
                std::memcpy(&regs.reg_array[Regs::ShRegWordOffset + set_data->reg_offset],
                            header + 2, set_size);
                // gow3: interleaves with the graphics stream at an unpredictable point, so
                // draw-preparation workers cannot reproduce it: make their checksums differ.
                gfx_reg_checksum = HashRegisterPacket(gfx_reg_checksum ^ 0xA5C0A5C0A5C0ull,
                                                      reinterpret_cast<const u32*>(header),
                                                      header->type3.NumWords() + 1);
            }
            break;
        }
        case PM4ItOpcode::SetQueueReg: {
            const auto* set_data = reinterpret_cast<const PM4CmdSetQueueReg*>(header);
            LOG_WARNING(Render, "Encountered compute SetQueueReg: vqid = {}, reg_offset = {:#x}",
                        set_data->vqid.Value(), set_data->reg_offset.Value());
            break;
        }
        case PM4ItOpcode::DispatchDirect: {
            const auto* dispatch_direct = reinterpret_cast<const PM4CmdDispatchDirect*>(header);
            if (auto it = std::ranges::find(indirect_patches, header, &IndirectPatch::header);
                it != indirect_patches.end()) {
                const auto size = sizeof(PM4CmdDispatchIndirect::GroupDimensions);
                if (acb_undrained) {
                    rasterizer->DrainDrawPipe(Vulkan::DrawPipe::ReasonCompute);
                }
                rasterizer->DispatchIndirect(it->indirect_addr, 0, size);
                break;
            }
            auto& cs_program = GetCsRegs();
            cs_program.dim_x = dispatch_direct->dim_x;
            cs_program.dim_y = dispatch_direct->dim_y;
            cs_program.dim_z = dispatch_direct->dim_z;
            cs_program.dispatch_initiator = dispatch_direct->dispatch_initiator;
            if (DebugState.DumpingCurrentReg()) {
                DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                               cs_program);
            }
            if (!rasterizer || (cs_program.dispatch_initiator & 1) == 0) {
                break;
            }
            const auto cmd_address = reinterpret_cast<const void*>(header);
            rasterizer->ScopeMarker("asc[{}]:{}:DispatchDirect",
                                    fmt::make_format_args(vqid, cmd_address),
                                    [&] { rasterizer->DispatchDirect(acb_undrained); });
            break;
        }
        case PM4ItOpcode::DispatchIndirect: {
            const auto* dispatch_indirect =
                reinterpret_cast<const PM4CmdDispatchIndirectMec*>(header);
            auto& cs_program = GetCsRegs();
            const auto ib_address = dispatch_indirect->Address<VAddr>();
            const auto size = sizeof(PM4CmdDispatchIndirect::GroupDimensions);
            if (DebugState.DumpingCurrentReg()) {
                DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                               cs_program);
            }
            if (!rasterizer || (cs_program.dispatch_initiator & 1) == 0) {
                break;
            }
            const auto cmd_address = reinterpret_cast<const void*>(header);
            rasterizer->ScopeMarker("asc[{}]:{}:DispatchIndirect",
                                    fmt::make_format_args(vqid, cmd_address),
                                    [&] { rasterizer->DispatchIndirect(ib_address, 0, size); });
            break;
        }
        case PM4ItOpcode::WriteData: {
            // gow3: copies deferred to the recording thread precede writes the guest sees, and
            // so do graphics fences deferred to it (the pipe was drained before this packet).
            if (rasterizer) {
                rasterizer->WaitHostCopies();
                rasterizer->WaitDeferredSignals();
            }
            const auto* write_data = reinterpret_cast<const PM4CmdWriteData*>(header);
            ASSERT(write_data->dst_sel.Value() == 2 || write_data->dst_sel.Value() == 5);
            const u32 data_size = (header->type3.count.Value() - 2) * 4;
            if (!write_data->wr_one_addr.Value()) {
                if (rasterizer) {
                    rasterizer->OnFence();
                }
                std::memcpy(write_data->Address<void*>(), write_data->data, data_size);
            } else {
                UNREACHABLE();
            }
            break;
        }
        case PM4ItOpcode::MemSemaphore: {
            const auto* mem_semaphore = reinterpret_cast<const PM4CmdMemSemaphore*>(header);
            if (rasterizer) {
                rasterizer->RingDedupClear();
            }
            if (mem_semaphore->IsSignaling()) {
                if (rasterizer) {
                    rasterizer->WaitRingCopies(); // R.9.12, as on the graphics queue
                }
                mem_semaphore->Signal();
            } else {
                while (!mem_semaphore->Signaled()) {
                    YIELD_ASC(vqid);
                }
                mem_semaphore->Decrement();
            }
            break;
        }
        case PM4ItOpcode::WaitRegMem: {
            const auto* wait_reg_mem = reinterpret_cast<const PM4CmdWaitRegMem*>(header);
            if (rasterizer) {
                rasterizer->RingDedupClear();
            }
            ASSERT(wait_reg_mem->engine.Value() == PM4CmdWaitRegMem::Engine::Me);
            const auto wait_started = std::chrono::steady_clock::now();
            bool wait_reported = false;
            while (!wait_reg_mem->Test(regs.reg_array)) {
                if (!wait_reported &&
                    std::chrono::steady_clock::now() - wait_started > std::chrono::seconds{2}) {
                    wait_reported = true;
                    ReportStuckWait(*wait_reg_mem, "compute");
                }
                YIELD_ASC(vqid);
            }
            break;
        }
        case PM4ItOpcode::ReleaseMem: {
            // uc3 (GOW3_HLE_INORDER): the fence also runs in order instead of draining.
            if (rasterizer && (ComputePipeOn() || inorder_skip)) {
                const ReleaseMemTask task{*reinterpret_cast<const PM4CmdReleaseMem*>(header),
                                          static_cast<u32>(queue.pipe_id)};
                const bool queued = rasterizer->RunInOrder(&RunReleaseMem, &task, sizeof(task));
                // R.9.2: a graphics WaitRegMem on this label is met in stream order.
                static const bool note_acb_fences = [] {
                    const char* env = std::getenv("GOW3_ACB_FENCE_NOTE");
                    return env && env[0] == '1';
                }();
                if (queued && note_acb_fences) {
                    NotePendingReleaseMem(task.packet);
                }
                rasterizer->NoteGuestWrite(task.packet.Address<VAddr>(), sizeof(u64));
                break;
            }
            // gow3: copies deferred to the recording thread precede writes the guest sees, and
            // so do graphics fences deferred to it (the pipe was drained before this packet).
            if (rasterizer) {
                rasterizer->WaitHostCopies();
                rasterizer->WaitDeferredSignals();
            }
            const auto* release_mem = reinterpret_cast<const PM4CmdReleaseMem*>(header);
            if (rasterizer) {
                rasterizer->OnFence();
            }
            release_mem->SignalFence(
                [pipe_id = queue.pipe_id] {
                    Platform::IrqC::Instance()->Signal(static_cast<Platform::InterruptId>(pipe_id));
                },
                [this](VAddr dst, u16 gds_index, u16 num_dwords) {
                    rasterizer->CopyBuffer(dst, gds_index, num_dwords * sizeof(u32), false, true);
                });
            break;
        }
        case PM4ItOpcode::EventWrite: {
            // const auto* event = reinterpret_cast<const PM4CmdEventWrite*>(header);
            break;
        }
        default:
            UNREACHABLE_MSG("Unknown PM4 type 3 opcode {:#x} with count {}",
                            static_cast<u32>(opcode), header->type3.NumWords());
        }
        // uc3 R.21: see ProcessGraphics.
        if (opcode == PM4ItOpcode::MemSemaphore || opcode == PM4ItOpcode::Rewind ||
            opcode == PM4ItOpcode::WaitRegMem) {
            Vulkan::SrtGlobalEvent(0);
        } else if ((opcode == PM4ItOpcode::DmaData || opcode == PM4ItOpcode::WriteData ||
                    opcode == PM4ItOpcode::CopyData || opcode == PM4ItOpcode::ReleaseMem) &&
                   Vulkan::g_srt_write_seq.load(std::memory_order_relaxed) == srt_seq_before &&
                   !(Vulkan::SrtA1() && SrtNotePacketRange(&header->type3))) {
            Vulkan::SrtGlobalEvent(3);
        }

        acb = NextPacket(acb, next_dw_off);

        if constexpr (!is_indirect) {
            *queue.read_addr += next_dw_off;
            *queue.read_addr %= queue.ring_size_dw;
        }
    }

    FIBER_EXIT;
}

Liverpool::CmdBuffer Liverpool::CopyCmdBuffers(std::span<const u32> dcb, std::span<const u32> ccb) {
    auto& queue = mapped_queues[GfxQueueId];
    ASSERT_MSG(queue.dcb_buffer.capacity() >= queue.dcb_buffer_offset + dcb.size(),
               "dcb copy buffer out of reserved space");
    ASSERT_MSG(queue.ccb_buffer.capacity() >= queue.ccb_buffer_offset + ccb.size(),
               "ccb copy buffer out of reserved space");

    queue.dcb_buffer.resize(
        std::max(queue.dcb_buffer.size(), queue.dcb_buffer_offset + dcb.size()));
    queue.ccb_buffer.resize(
        std::max(queue.ccb_buffer.size(), queue.ccb_buffer_offset + ccb.size()));

    const u32 prev_dcb_buffer_offset = queue.dcb_buffer_offset;
    const u32 prev_ccb_buffer_offset = queue.ccb_buffer_offset;
    if (!dcb.empty()) {
        std::memcpy(queue.dcb_buffer.data() + queue.dcb_buffer_offset, dcb.data(),
                    dcb.size_bytes());
        queue.dcb_buffer_offset += dcb.size();
        dcb = std::span<const u32>{queue.dcb_buffer.begin() + prev_dcb_buffer_offset,
                                   queue.dcb_buffer.begin() + queue.dcb_buffer_offset};
    }

    if (!ccb.empty()) {
        std::memcpy(queue.ccb_buffer.data() + queue.ccb_buffer_offset, ccb.data(),
                    ccb.size_bytes());
        queue.ccb_buffer_offset += ccb.size();
        ccb = std::span<const u32>{queue.ccb_buffer.begin() + prev_ccb_buffer_offset,
                                   queue.ccb_buffer.begin() + queue.ccb_buffer_offset};
    }

    return std::make_pair(dcb, ccb);
}

void Liverpool::SubmitGfx(std::span<const u32> dcb, std::span<const u32> ccb) {
    auto& queue = mapped_queues[GfxQueueId];

    if (EmulatorSettings.IsCopyGpuBuffers()) {
        std::tie(dcb, ccb) = CopyCmdBuffers(dcb, ccb);
    }

    // The copy for the draw preparation workers is made before taking the queue lock, which
    // the GPU thread needs to pick up work.
    auto* draw_prep = rasterizer ? &rasterizer->GetDrawPreparation() : nullptr;
    auto prep_submission = draw_prep ? draw_prep->Build(dcb) : nullptr;
    {
        // Numbering, enqueueing and queueing under one lock keep the three orders identical.
        std::scoped_lock lock{queue.m_access};
        u64 seq = NoSeq;
        if (draw_prep) {
            seq = gfx_submit_seq++;
            draw_prep->Enqueue(seq, std::move(prep_submission));
        }
        auto task = ProcessGraphics(dcb, ccb, seq);
        queue.submits.emplace(task.handle);
    }

    std::scoped_lock lk{submit_mutex};
    ++num_submits;
    work_retired = false;
    submit_cv.notify_one();
}

void Liverpool::SubmitAsc(u32 gnm_vqid, std::span<const u32> acb) {
    ASSERT_MSG(gnm_vqid > 0 && gnm_vqid < NumTotalQueues, "Invalid virtual ASC queue index");
    auto& queue = mapped_queues[gnm_vqid];

    const auto vqid = gnm_vqid - 1;
    const auto& task = ProcessCompute(acb, vqid);
    {
        std::scoped_lock lock{queue.m_access};
        queue.submits.emplace(task.handle);
    }

    std::scoped_lock lk{submit_mutex};
    num_mapped_queues = std::max(num_mapped_queues, gnm_vqid + 1);
    ++num_submits;
    work_retired = false;
    submit_cv.notify_one();
}

} // namespace AmdGpu
