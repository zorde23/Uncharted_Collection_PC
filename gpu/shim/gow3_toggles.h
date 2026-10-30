// gow3: optimizations that can be switched off while the game runs (GOW3_TOGGLE_FILE,
// see runtime_memory.c), to find which one changes rendering without restarting.
#pragma once
#include <atomic>
#include <csetjmp>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#ifdef _WIN32
// gow3 (Windows): no signals. The loader's exception handler resumes the faulting thread
// in gow3_longjmp (src/compat_win.c: no unwinding through the faulting frames).
typedef unsigned long long sigjmp_buf[32];
extern "C" __attribute__((returns_twice)) int gow3_setjmp(sigjmp_buf buffer);
#define sigsetjmp(buffer, save) gow3_setjmp(buffer)
#endif

extern "C" std::uint64_t runtime_disabled_optimizations;
/// Recovery point for speculative guest memory reads on this thread (runtime_memory.c).
extern "C" __thread sigjmp_buf* runtime_fault_recover;

namespace Gow3Toggle {
enum : std::uint64_t {
    RegionCache = 1,
    FetchShaderCache = 2,
    PageTrackingEarlyExit = 4,
    PendingPollLimit = 8,
    ThreadedRecording = 16,
    ImageDescCache = 32,
    LockFreeUploadCheck = 64,
    FindImageCache = 128,
    DeferredUploads = 256,
    AccessMemo = 512,
    TextureBindingMemo = 1024,
    CoarseReadTracking = 2048,
    PreparedResources = 4096,
    DrawPreparation = 8192,
    DeferredStreamCopies = 16384,
    HotPages = 32768,
    FaultWindow = 65536,
    ParallelCopies = 131072,
    AsyncFences = 262144,
    PoolSmallCopies = 524288,
    RecordPrefetch = 1ull << 32,
    TextureViewMemo = 1ull << 33,
    TextureBindHelper = 1ull << 34,
    EarlyDrawInputs = 1ull << 35,
    ConstantRing = 1ull << 36,
    DrawPipeline = 1ull << 37,
    PipelinedTasks = 1ull << 38,
    PendingFenceWaits = 1ull << 39,
    PipelinedDispatch = 1ull << 40,
    RecorderFences = 1ull << 41,
    PipelinedMemoryWrites = 1ull << 42,
    MultiCopyShader = 1ull << 43,
    RenderStateMemo = 1ull << 44,
    TextureSetMemo = 1ull << 45,
    PipelinedIndirectDraws = 1ull << 46,
    SceneAttachmentsOnly = 1ull << 47,
    SampleSceneProxies = 1ull << 48,
    OrderedGuestWrites = 1ull << 49,
    SceneHalfRes = 1ull << 50, ///< live scaling also reduces the 960x540 post targets
    UpdateImageFastPath = 1u << 30,
    // TAA A/B in one run: optional techniques, off by default (no measured gain, 2026-10-02).
    TaaTonemapBlend = 1ull << 51,
    TaaClip = 1ull << 52,
    TaaVariance = 1ull << 53,
    TaaFilter = 1ull << 54,
    // On by default (bit set: off): history of a thin feature this jitter phase missed is kept
    // when nothing moves. Static-camera flicker of railings/window bars p99.9 -45% (2026-10-03).
    TaaKeepNearerHistory = 1ull << 55,
    SceneMipBias = 1ull << 57, ///< negative LOD bias of G-buffer samplers at reduced scene sizes
    // Bits 20-29 are used as raw debug toggles by the camera/object motion and the upscaler.
};
inline bool Disabled(std::uint64_t bit) {
    return (__atomic_load_n(&runtime_disabled_optimizations, __ATOMIC_RELAXED) & bit) != 0;
}
} // namespace Gow3Toggle

namespace Gow3Stats {
/// Guest writes caught by page protection, and pages currently left unprotected as hot.
inline std::atomic<std::uint64_t> tracker_faults{0};
inline std::atomic<std::int64_t> hot_pages{0};
/// Stall diagnostics (GOW3_FRAME_STATS): per-frame deltas printed for frames over 40 ms.
inline std::atomic<std::uint64_t> images_registered{0};
inline std::atomic<std::uint64_t> image_upload_bytes{0};
inline std::atomic<std::uint64_t> buffer_upload_bytes{0};
inline std::atomic<int> gpu_thread_clock{-1}; ///< clockid_t of the GPU command thread
inline std::atomic<unsigned long> gpu_thread_id{0}; ///< Windows thread id of the GPU command thread
inline std::atomic<std::uint64_t> draws{0}, dispatches{0}, submissions{0};
/// uc3: draws left out because no pipeline came back (any reason), direct and indirect, and
/// indirect draws seen (GOW3_FRAME_STATS).
inline std::atomic<std::uint64_t> uc3_no_pipeline_draws{0}, uc3_no_pipeline_indirect{0}, uc3_indirect_draws{0};
/// uc3: precise readbacks that brought back every GPU-modified range at once, and their bytes.
inline std::atomic<std::uint64_t> uc3_readback_all{0}, uc3_readback_all_bytes{0};
/// uc3: precise readbacks with GOW3_READBACK_HOT=1, and the hot windows they brought back
/// along with the requested one (each one a later fault avoided if the CPU reads it again).
inline std::atomic<std::uint64_t> uc3_readback_hot{0}, uc3_readback_hot_windows{0};
// uc3: precise readback faults by where the last GPU write to the window stands when the fault
// comes: still in the open command buffer, submitted but not finished, already finished, or
// never seen. Decides whether waiting for the write's own tick (not the current one) would pay.
inline std::atomic<std::uint64_t> uc3_rb_open{0}, uc3_rb_inflight{0}, uc3_rb_done{0},
    uc3_rb_unknown{0}, uc3_rb_inflight_ticks{0};
// uc3: GOW3_READBACK_FAST downloads: done on the side (no queue drain) / the full wait, and the
// time spent waiting for the side copies.
inline std::atomic<std::uint64_t> uc3_rb_side{0}, uc3_rb_side_ns{0}, uc3_rb_full{0}, uc3_rb_side_waits{0};
// gow3 R.9.7: steady_clock ns when the latest flip packet was decoded (stage A) and reached the
// recording side (stage B, SignalFlip); the stall log measures them from the previous flip.
inline std::atomic<std::int64_t> flip_a_ns{0}, flip_b_ns{0}, flip_game_ns{0};
// Per flip (ring of 16 by flip number): game submit, stage A decode, stage B reached.
inline std::atomic<std::int64_t> flip_ring[3][16]{};
inline std::atomic<std::uint32_t> flip_seq[3]{};
// uc2 (train, GOW3_READBACK_VALUE): buffer stores recorded into the GOW3_WATCH_ADDR range.
inline std::atomic<std::uint64_t> watch_store_count{0};
inline void NoteFlipStage(int stage) {
    const std::int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::uint32_t n = flip_seq[stage].fetch_add(1, std::memory_order_relaxed);
    flip_ring[stage][n % 16].store(now, std::memory_order_relaxed);
}
// uc3: GOW3_READBACK_ASYNC read faults: downloads submitted without draining the GPU thread, the
// faulting threads' wait for them, ones left protected because the GPU wrote the window again
// meanwhile, and GPU-side waits for a readback still in flight on the same window.
inline std::atomic<std::uint64_t> uc3_rb_async{0}, uc3_rb_async_ns{0}, uc3_rb_async_dirty{0},
    uc3_rb_async_overlap{0};
// uc3: readbacks still done the synchronous way: faults on GPU-side threads, write faults with
// async off, and the rest (read faults with async off, unmaps).
inline std::atomic<std::uint64_t> uc3_rb_sync_gpu{0}, uc3_rb_sync_write{0}, uc3_rb_sync_other{0};
// uc3 (GOW3_FAULT_TRACE=1): address of the last GPU-side read fault, for GOW3_WRITE_TRACE=1.
inline std::atomic<std::uint64_t> uc3_last_gpu_fault{0};
// uc3: SRT walker loads on protected pages done from the backing view (bytes not GPU-written),
// and those that still took the synchronous readback.
inline std::atomic<std::uint64_t> uc3_srt_emulated{0}, uc3_srt_sync{0}, uc3_srt_stale{0};
// uc3 (R.8.1): walker pointers / leaf dwords read back after the walk, walks that did.
inline std::atomic<std::uint64_t> uc3_srt_ptr_sync{0}, uc3_srt_leaf_sync{0}, uc3_srt_sync_walks{0};
// uc3 (R.8.2): GPU-written dwords left 0 for a GPU copy, sharps read back, copies recorded.
inline std::atomic<std::uint64_t> uc3_srt_leaf_zeroed{0}, uc3_srt_leaf_sharp{0}, uc3_leaf_copies{0},
    uc3_leaf_noring{0}, uc3_leaf_dead{0}, uc3_ring_full{0}, uc3_ring_copies{0},
    uc3_leaf_by_window{0}, uc3_leaf_new_addr{0}, uc3_leaf_dl_queued{0}, uc3_leaf_dl_landed{0},
    uc3_leaf_dl_dirty{0}, uc3_leaf_dl_full{0};
// uc3: GPU cost counters (Fable R.3): render passes actually ended, memory barriers recorded by
// FlushBarriers and how many of them wait on graphics stages, layout transitions that produced
// a barrier and how many on images of 1920x1080 or more.
inline std::atomic<std::uint64_t> uc3_pass_ends{0}, uc3_barriers{0}, uc3_barriers_gfx{0},
    uc3_transits{0}, uc3_transits_big{0};
/// Frames the GPU command thread has started (display pass), for per-frame diagnostics.
inline std::atomic<std::uint64_t> gpu_frames{0};
/// Wall time spent in operations suspected of stalls (ns, all threads).
inline std::atomic<std::uint64_t> t_resident{0}, t_protect{0}, t_image_create{0}, t_refresh{0},
    t_staging{0}, t_host_wait{0}, t_copy{0}, copy_bytes{0}, t_read_faults{0}, read_faults{0},
    t_write_faults{0}, t_copy_cpu{0}, copy_sys_us{0}, copy_minflt{0}, t_gc{0}, t_readback_wait{0};
/// uc3 (Fable A4.2b): readback commands from the command queue (count, ns), and inside the
/// readback work on GPU-side threads: texture cache resolves (count, ns), buffer cache flushes
/// (count, ns) split into waiting for async readbacks and recording the downloads.
inline std::atomic<std::uint64_t> cmd_count{0}, cmd_ns{0}, resolve_count{0}, resolve_ns{0},
    flush_count{0}, flush_ns{0}, flush_wait_ns{0}, flush_download_ns{0};
/// uc3 (Fable A4): scalar copy sizes, bucketed (<=256, <=4K, <=64K, <=1M, >1M): copies refused
/// by the in-order path for size, and copies handed to the recording thread (GOW3_HLE_BIG_PIPE).
inline std::atomic<std::uint64_t> size_reject_hist[5]{}, big_pipe_hist[5]{};
inline unsigned SizeBucket(std::uint64_t size) {
    return size <= 256 ? 0 : size <= 4096 ? 1 : size <= 65536 ? 2 : size <= (1u << 20) ? 3 : 4;
}
/// uc3 (GOW3_B_CENSUS=1, diagnostic, Fable R.14): what stage B spends its time on. Tasks by
/// function (named by the file that posts them), and how many recording kicks come from
/// SignalAfterHostCopies.
inline const bool b_census = [] {
    const char* env = std::getenv("GOW3_B_CENSUS");
    return env && env[0] == '1';
}();
struct TaskStat {
    std::atomic<const void*> fn{nullptr};
    const char* name = nullptr;
    std::atomic<std::uint64_t> count{0}, ns{0};
};
inline TaskStat task_stats[16];
inline TaskStat* TaskSlot(const void* fn) {
    for (auto& slot : task_stats) {
        const void* cur = slot.fn.load(std::memory_order_relaxed);
        if (cur == fn) {
            return &slot;
        }
        if (!cur) {
            const void* expected = nullptr;
            if (slot.fn.compare_exchange_strong(expected, fn) || expected == fn) {
                return &slot;
            }
        }
    }
    return nullptr;
}
inline void NameTask(const void* fn, const char* name) {
    if (auto* slot = TaskSlot(fn)) {
        slot->name = name;
    }
}
inline std::atomic<std::uint64_t> signal_after_copies_kicks{0};
/// Fable R.14 D0: guest frames (OnSubmit), the shader stage B is recording (its write marks
/// the pages it binds as written; 1 = a draw, else a dispatch's hash or a task's function),
/// and the recorder thread: kicks, chunks and bytes per kick, kicks that woke it, time busy.
inline std::atomic<std::uint64_t> frame_ticks{0}, diag_writer{0};
inline std::atomic<std::uint64_t> rec_kicks{0}, rec_kick_chunks{0}, rec_kick_bytes{0},
    rec_wakes{0}, rec_exec_ns{0};
/// Fable R.14 D1 (GOW3_LAZY_FENCE_KICK=1, off by default): a fence handed to the recording
/// thread no longer forces a kick. It leaves with the next kick (32 KiB, drains, waits, flip),
/// and stage B kicks when it runs out of packets while a fence is still unkicked. D3 (GOW3_FENCE_BATCH=1,
/// needs D1): the fences of one kick run in one closure. Counted always, printed with
/// GOW3_B_CENSUS: kicks that carried fences (forced or natural), the fences they carried, and
/// the kicks stage B made going idle.
inline const bool lazy_fence_kick = [] {
    const char* env = std::getenv("GOW3_LAZY_FENCE_KICK");
    return env && env[0] == '1';
}();
inline const bool fence_batch = lazy_fence_kick && [] {
    const char* env = std::getenv("GOW3_FENCE_BATCH");
    return env && env[0] == '1';
}();
inline std::atomic<std::uint64_t> fence_kicks_forced{0}, fences_forced{0},
    fence_kicks_natural{0}, fences_natural{0}, idle_kicks{0};
/// Fable R.14 D4 diagnostics (GOW3_D4_DIAG=1, off by default): DmaData immediate fills to
/// memory on the CPU or the GPU branch (and how many GPU-branch fills had their own bytes
/// GPU-written), and why the done-window fast readback refused the faults of the GfxEop
/// handler, NdJobWorkerThread 0 and the other threads.
inline const bool d4_diag = [] {
    const char* env = std::getenv("GOW3_D4_DIAG");
    return env && env[0] == '1';
}();
inline std::atomic<std::uint64_t> fill_cpu{0}, fill_gpu{0}, fill_gpu_own_bytes{0};
inline std::atomic<std::uint64_t> fast_reason[3][11]{};
/// Diagnostics are collected only with GOW3_FRAME_STATS=1.
inline const bool enabled = [] {
    const char* env = std::getenv("GOW3_FRAME_STATS");
    return env && env[0] == '1';
}();
struct Timer {
    std::atomic<std::uint64_t>& total;
    std::chrono::steady_clock::time_point start =
        enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    ~Timer() {
        if (!enabled) {
            return;
        }
        total.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count(),
                        std::memory_order_relaxed);
    }
};
/// Wall time the GPU command thread waited for guest submissions (ns).
inline std::atomic<std::uint64_t> gpu_idle_ns{0};
/// Draws recorded into the reduced scene targets, and draws after the scene started.
inline std::atomic<std::uint64_t> reduced_draws{0}, scene_draws{0};
/// Wall time spent blocked in the scheduler (ns): waiting for the recording thread to drain,
/// for host copies before a submission or fence, and for GPU ticks.
inline std::atomic<std::uint64_t> sync_recording_ns{0}, host_copies_wait_ns{0}, tick_wait_ns{0},
    copy_threads_wait_ns{0}, host_copy_waits{0};
struct WaitTimer {
    std::atomic<std::uint64_t>& total;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~WaitTimer() {
        total.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start).count(),
                        std::memory_order_relaxed);
    }
};
/// GPU thread rusage, refreshed after each graphics submission.
inline std::atomic<std::uint64_t> gpu_sys_us{0}, gpu_user_us{0}, gpu_invol_switches{0},
    gpu_vol_switches{0}, gpu_minor_faults{0};
/// Protection faults (signals) taken by the GPU thread itself.
inline std::atomic<std::uint64_t> gpu_signal_faults{0};
/// Protection changes: calls and pages, those removing write access (TLB shootdowns) apart.
inline std::atomic<std::uint64_t> protect_calls{0}, protect_pages{0}, protect_revoke_calls{0},
    protect_revoke_pages{0};
} // namespace Gow3Stats
