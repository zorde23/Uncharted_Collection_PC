// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: speculative draw preparation on worker threads (docs/parallel_gpu.md, steps 1-2).
//
// Every submitted graphics command buffer is copied. A scanner thread replays the register
// writes of the whole stream in order (ApplyGraphicsRegisterPacket, the same code the GPU
// thread runs) and records, per buffer, its starting checksum and a delta of the register
// blocks it wrote. Any number of workers (one per spare hardware thread) claim the nearest
// buffers ahead of the GPU thread, reach their starting state by applying deltas, and select
// the graphics pipeline and resource sharps of each direct draw. All helpers run as
// SCHED_IDLE: they only use idle cores. The GPU thread uses a prepared draw only when the
// running register checksums match and the flattened user data it computes itself equals the
// worker's; otherwise it takes the regular path. Workers never create or compile anything.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include "common/types.h"
#include "shader_recompiler/ir/passes/srt.h"
#include "video_core/amdgpu/regs.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"

namespace Vulkan {

struct PreparedStage {
    const Program* program;
    u64 hash;
    Shader::HwStage hw_stage;
    VAddr pgm_base;
    const u32* flat; ///< flattened user data the worker computed, owned by the submission
    u32 flat_size;
    // Resource sharps read from `flat` in the order of the program's resource lists, and the
    // texture description hash of each image (docs/parallel_gpu.md, step 2). Valid for the
    // GPU thread exactly when `flat` matched: sharps depend on nothing else.
    const AmdGpu::Image* image_sharps;
    const u64* image_hashes;
    const AmdGpu::Sampler* sampler_sharps;
    const AmdGpu::Buffer* buffer_sharps;
    u32 num_images, num_samplers, num_buffers;
};

/// Key of Rasterizer's texture description cache for a T# bound through `res`.
inline u64 ImageDescHash(const AmdGpu::Image& sharp, const Shader::ImageResource& res) {
    std::array<u64, 4> key;
    std::memcpy(key.data(), &sharp, sizeof(key));
    const u32 flags = u32(res.is_written) | u32(res.is_depth) << 1 | u32(res.is_array) << 2;
    u64 hash = flags * 0x9E3779B97F4A7C15ull;
    for (const u64 word : key) {
        hash = (hash ^ word) * 0xFF51AFD7ED558CCDull;
        hash ^= hash >> 32;
    }
    return hash;
}

/// Vertex inputs of a draw (the dynamic vertex input path): the attribute and binding
/// descriptions, the V# of each stream, and the stream memory merged into ranges.
struct PreparedVertexInputs {
    struct Range {
        VAddr base, end;
    };
    const vk::VertexInputAttributeDescription2EXT* attributes;
    const vk::VertexInputBindingDescription2EXT* bindings;
    const AmdGpu::Buffer* buffers;
    const u8* range_index; ///< per stream with memory: its entry in `ranges`
    const Range* ranges;
    u32 count, num_ranges;
    u64 buffers_hash; ///< XXH3 of `buffers` (object motion stream identity)
    bool valid;
};

/// uc3 R.20/R.21 (GOW3_PREP_TRUST): which guest memory changed since a prepared draw's worker
/// walked its resource tables. Every command-stream write and every written binding of a draw
/// or dispatch takes a sequence number and stamps the pages it covers (direct-mapped and
/// untagged, so a collision only invalidates more). Writes over 64 pages and events that can
/// release CPU-written data (waits) move the big/global stamps instead.
inline constexpr u32 SrtPageSlots = 1u << 16;
inline std::atomic<u64> g_srt_write_seq{0};
inline std::atomic<u64> g_srt_global_seq{0};
inline std::atomic<u64> g_srt_page_seq[SrtPageSlots];
/// Diagnostics: 0 waits (global), 1 page writes, 2 big writes, 3 packets that wrote without a
/// noted range (global).
inline std::atomic<u64> g_srt_bumps[4];
/// uc3 R.10 A1 (Fable): stamps only move forward, so writers on several threads cannot lower
/// a stamp a later write already raised.
inline void SrtStoreMax(std::atomic<u64>& stamp, u64 seq) noexcept {
    u64 current = stamp.load(std::memory_order_relaxed);
    while (current < seq &&
           !stamp.compare_exchange_weak(current, seq, std::memory_order_release,
                                        std::memory_order_relaxed)) {
    }
}
inline void SrtGlobalEvent(u32 source = 0) noexcept {
    const u64 seq = g_srt_write_seq.fetch_add(1, std::memory_order_acq_rel) + 1;
    SrtStoreMax(g_srt_global_seq, seq);
    g_srt_bumps[source].fetch_add(1, std::memory_order_relaxed);
}
/// uc3 R.10 A1 (b, Fable): big writes stamp regions of 64 pages (256 KB) like the pages, so a
/// big write only invalidates the prepared draws that read inside it, with any number of
/// writers and no scan. Direct-mapped and untagged: a collision only invalidates more. Writes
/// over SrtRegionSlots regions move the global stamp.
inline constexpr u32 SrtRegionSlots = 1u << 16;
inline std::atomic<u64> g_srt_region_seq[SrtRegionSlots];
/// R.10 A1 v8 (Fable, diagnostics): who stamped each page slot last. Racy and untagged like
/// the stamps; it only feeds the counters of the page write misses.
enum SrtOrigin : u8 {
    SrtOriginOther,
    SrtOriginPacket,     ///< a memory-writing packet, on the command thread
    SrtOriginGuestWrite, ///< Rasterizer::NoteGuestWrite (before a host-side write)
    SrtOriginPendingGpu, ///< Rasterizer::NotePendingGpuWrite
    SrtOriginLanding,    ///< a PM4 write landing (DmaData, WriteData, fences)
    SrtOriginBinding,    ///< a written buffer binding of a draw or dispatch
    SrtOriginRewrite,    ///< stamped again once written (DMA copy, HLE, constant RAM)
    SrtOriginReadback,   ///< GPU-written bytes landing from a readback (R.10.17)
    SrtNumOrigins,
};
inline std::atomic<u8> g_srt_page_origin[SrtPageSlots];
/// uc3 R.10 A1 (GOW3_PREP_TRUST_A1=1, Fable): prep trust made sound without the value
/// re-check. Writes are stamped again where they land (stage B / recording thread), the worker
/// flags GPU-written dwords its walk read, graphics waits are evaluated by the worker (a draw
/// after a wait not yet met is not trusted) instead of moving the global stamp, and packets
/// that write guest memory stamp their own range.
inline bool SrtA1() {
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_PREP_TRUST_A1");
        return env && env[0] == '1';
    }();
    return enabled;
}
/// R.10 A1 v9 (Fable): the last writes of each page slot with their range in the page, so a
/// page write can be told apart from one that hit the bytes a prepared draw read. Every entry
/// a writer replaces goes to `evicted` (max seq), so with no writer in flight (done == count)
/// the writes since a snapshot are the entries after it, or `evicted` is after it. Entry:
/// seq (40 bits) | first byte (12) | last byte (12). Untagged like the stamps: writes of an
/// aliased page only count as more hits.
inline constexpr u32 SrtPageRing = 8;
struct SrtPageHistory {
    std::atomic<u32> count;
    std::atomic<u32> done;
    std::atomic<u64> evicted;
    std::atomic<u64> entries[SrtPageRing];
};
inline SrtPageHistory g_srt_page_history[SrtPageSlots];
inline constexpr u64 SrtSeqMask = (u64(1) << 40) - 1;
inline void SrtNoteWrite(u64 address, u64 size, u8 origin = SrtOriginOther) noexcept {
    if (size == 0) {
        return;
    }
    const u64 seq = g_srt_write_seq.fetch_add(1, std::memory_order_acq_rel) + 1;
    const u64 first = address >> 12, last = (address + size - 1) >> 12;
    if (last - first >= 64) {
        const u64 first_region = first >> 6, last_region = last >> 6;
        if (last_region - first_region >= SrtRegionSlots) {
            SrtStoreMax(g_srt_global_seq, seq);
        } else {
            for (u64 region = first_region; region <= last_region; ++region) {
                SrtStoreMax(g_srt_region_seq[region % SrtRegionSlots], seq);
            }
        }
        g_srt_bumps[2].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const bool history = SrtA1();
    for (u64 page = first; page <= last; ++page) {
        if (history) {
            const u64 base = page << 12;
            const u64 begin = std::max(address, base) - base;
            const u64 end = std::min(address + size, base + 4096) - base - 1;
            auto& slot = g_srt_page_history[page % SrtPageSlots];
            const u32 n = slot.count.fetch_add(1, std::memory_order_acq_rel);
            const u64 old = slot.entries[n % SrtPageRing].exchange(
                (end << 52) | (begin << 40) | (seq & SrtSeqMask), std::memory_order_acq_rel);
            SrtStoreMax(slot.evicted, old & SrtSeqMask);
            slot.done.fetch_add(1, std::memory_order_release);
        }
        SrtStoreMax(g_srt_page_seq[page % SrtPageSlots], seq);
        g_srt_page_origin[page % SrtPageSlots].store(origin, std::memory_order_relaxed);
    }
    g_srt_bumps[1].fetch_add(1, std::memory_order_relaxed);
}
/// A write landing in guest memory (stage B, the recording thread, a copy thread).
inline void SrtNoteLanding(u64 address, u64 size, u8 origin = SrtOriginLanding) noexcept {
    if (!SrtA1() || size == 0) {
        return;
    }
    SrtNoteWrite(address, size, origin);
}
/// uc3 R.10.17 (Fable): GPU-written bytes landing in guest memory from a readback (buffer or
/// image) stamp their pages. The stamp at emission says a write happened, not that it landed: a
/// worker reading before the landing walked the old bytes. Part of A1 (on with
/// GOW3_PREP_TRUST_A1=1); GOW3_A1_READBACK_STAMP=0 turns it off for a test.
inline bool SrtReadbackStamp() {
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_A1_READBACK_STAMP");
        return env ? std::atoi(env) != 0 : SrtA1();
    }();
    return enabled;
}
inline void SrtNoteReadback(u64 address, u64 size) noexcept {
    if (SrtReadbackStamp()) {
        SrtNoteLanding(address, size, SrtOriginReadback);
    }
}
/// uc3 R.10 A1 (a): labels written by graphics packets (EOP/EOS/ReleaseMem/WriteData of a
/// label), with the value and the submission, noted by the prep worker while it walks a
/// submission. A wait the worker finds not met yet is GPU-GPU when a label write of the same
/// submission, or of an earlier one the GPU thread has not started, meets it: it does not
/// protect CPU writes (GPU outputs are on the page stamps and the GPU-leaf check), so the
/// draws after it stay trusted. Values from submissions already run do not count (a label CPU
/// and GPU both write). Direct-mapped: a lost entry only leaves a wait counted as CPU.
inline constexpr u32 SrtLabelSlots = 1u << 12;
struct SrtGpuLabel {
    std::atomic<u64> address{0};
    std::atomic<u64> value{0};
    std::atomic<u64> seq{0};
};
inline SrtGpuLabel g_srt_gpu_labels[SrtLabelSlots];
inline void SrtNoteGpuLabel(u64 address, u64 value, u64 seq) noexcept {
    auto& slot = g_srt_gpu_labels[(address >> 2) % SrtLabelSlots];
    slot.address.store(0, std::memory_order_relaxed);
    slot.value.store(value, std::memory_order_relaxed);
    slot.seq.store(seq, std::memory_order_relaxed);
    slot.address.store(address, std::memory_order_release);
}
inline bool SrtGpuLabelValue(u64 address, u64& value, u64& seq) noexcept {
    const auto& slot = g_srt_gpu_labels[(address >> 2) % SrtLabelSlots];
    if (slot.address.load(std::memory_order_acquire) != address) {
        return false;
    }
    value = slot.value.load(std::memory_order_relaxed);
    seq = slot.seq.load(std::memory_order_relaxed);
    return slot.address.load(std::memory_order_acquire) == address;
}
/// Diagnostics: waits the worker found not met, of them GPU-GPU.
inline std::atomic<u64> g_srt_waits_unmet{0}, g_srt_waits_gpu{0};
/// R.10 A1: waits that left draws after a wait: [0] WaitRegMem on a CPU label, [1] MemSemaphore,
/// [2] Rewind; and the most frequent CPU wait sites (label address and compare function).
inline std::atomic<u64> g_srt_wait_kinds[3];
struct SrtWaitSite {
    u64 address;
    u32 function;
    u32 ref;
    u64 count;
};
inline std::mutex g_srt_wait_sites_mutex;
inline std::array<SrtWaitSite, 64> g_srt_wait_sites{};
inline u32 g_srt_num_wait_sites = 0;
inline u64 g_srt_wait_sites_lost = 0;
inline void SrtNoteCpuWait(u64 address, u32 function, u32 ref) noexcept {
    std::scoped_lock lk{g_srt_wait_sites_mutex};
    for (u32 i = 0; i < g_srt_num_wait_sites; ++i) {
        auto& site = g_srt_wait_sites[i];
        if (site.address == address && site.function == function) {
            site.ref = ref;
            ++site.count;
            return;
        }
    }
    if (g_srt_num_wait_sites == g_srt_wait_sites.size()) {
        ++g_srt_wait_sites_lost;
        return;
    }
    g_srt_wait_sites[g_srt_num_wait_sites++] = {address, function, ref, 1};
}

/// Set by the SRT fixup hook when a walk off the GPU-side threads (the prep worker) read a
/// GPU-written dword.
inline thread_local bool g_srt_walk_gpu_leaf = false;

/// 0: unchanged since `snap`; 1 only a global event (R.22: the values may still be checked),
/// 2 a big write, 3 a page write.
inline u32 SrtChangedSince(u64 snap, const u64* pages, u32 count) noexcept {
    for (u32 i = 0; i < count; ++i) {
        if (g_srt_page_seq[pages[i] % SrtPageSlots].load(std::memory_order_acquire) > snap) {
            return 3;
        }
        if (g_srt_region_seq[(pages[i] >> 6) % SrtRegionSlots].load(std::memory_order_acquire) >
            snap) {
            return 2;
        }
    }
    if (g_srt_global_seq.load(std::memory_order_acquire) > snap) {
        return 1;
    }
    return 0;
}
/// R.10 A1 v9 (Fable): how the writes to `page` since `snap` meet the guest ranges a draw
/// read: 0 none of them hits, 1 one hits, 2 unknown (history lost, writer in flight, or no
/// range read on the page).
inline u32 SrtPageWritesHit(u64 page, u64 snap, const Shader::SrtGuestRun* runs,
                            u32 num_runs) noexcept {
    const u64 base = page << 12;
    bool reads_page = false;
    for (u32 r = 0; r < num_runs; ++r) {
        reads_page |= runs[r].guest < base + 4096 && runs[r].guest + runs[r].size > base;
    }
    if (!reads_page) {
        return 2;
    }
    auto& slot = g_srt_page_history[page % SrtPageSlots];
    const u32 done = slot.done.load(std::memory_order_acquire);
    if (slot.count.load(std::memory_order_acquire) != done) {
        return 2;
    }
    u64 entries[SrtPageRing];
    for (u32 j = 0; j < SrtPageRing; ++j) {
        entries[j] = slot.entries[j].load(std::memory_order_acquire);
    }
    const u64 evicted = slot.evicted.load(std::memory_order_acquire);
    if (slot.done.load(std::memory_order_acquire) != done ||
        slot.count.load(std::memory_order_acquire) != done) {
        return 2;
    }
    const u64 since = snap & SrtSeqMask;
    if (evicted > since) {
        return 2;
    }
    bool any = false;
    for (const u64 entry : entries) {
        if ((entry & SrtSeqMask) <= since) {
            continue;
        }
        any = true;
        const u64 begin = base + ((entry >> 40) & 0xFFF), end = base + (entry >> 52) + 1;
        for (u32 r = 0; r < num_runs; ++r) {
            if (runs[r].guest < end && runs[r].guest + runs[r].size > begin) {
                return 1;
            }
        }
    }
    return any ? 0 : 2;
}

/// R.10 A1 v9 (Fable): SrtChangedSince where the page writes that miss the ranges read do not
/// count. `page_hits`: 0 every page write missed them, 1 one hit, 2 unknown (as
/// SrtPageWritesHit); untouched when no page was written.
inline u32 SrtChangedSinceRanges(u64 snap, const u64* pages, u32 count,
                                 const Shader::SrtGuestRun* runs, u32 num_runs,
                                 u32& page_hits) noexcept {
    u32 hits = 0;
    bool written = false;
    for (u32 i = 0; i < count; ++i) {
        if (g_srt_page_seq[pages[i] % SrtPageSlots].load(std::memory_order_acquire) > snap) {
            written = true;
            hits = std::max(hits, SrtPageWritesHit(pages[i], snap, runs, num_runs));
            if (hits != 0) {
                page_hits = hits;
                return 3;
            }
        }
        if (g_srt_region_seq[(pages[i] >> 6) % SrtRegionSlots].load(std::memory_order_acquire) >
            snap) {
            return 2;
        }
    }
    if (written) {
        page_hits = 0;
    }
    if (g_srt_global_seq.load(std::memory_order_acquire) > snap) {
        return 1;
    }
    return 0;
}

/// R.10 A1 v8 (diagnostics): the index of the first page stamped since `snap`, or `count`.
inline u32 SrtChangedPage(u64 snap, const u64* pages, u32 count) noexcept {
    for (u32 i = 0; i < count; ++i) {
        if (g_srt_page_seq[pages[i] % SrtPageSlots].load(std::memory_order_acquire) > snap) {
            return i;
        }
    }
    return count;
}

/// uc3 R.10 A1 (Fable): a wait the worker found met before a draw: re-tested at the draw when
/// the page of its label was written since the snapshot (labels share pages densely).
struct SrtMetWait {
    u64 address;
    u32 ref;
    u32 mask;
    u8 function;  ///< PM4CmdWaitRegMem::Function
    u8 semaphore; ///< MemSemaphore wait: met while the 64-bit value is not 0
};

struct PreparedDraw {
    enum : u32 { Pending = 0, Ready = 1, Unavailable = 2 };
    std::atomic<u32> state{Pending};
    u64 reg_checksum{};
    u64 srt_snap{};        ///< R.21: g_srt_write_seq read before the worker's walk
    bool no_gpu_leaves{};  ///< R.20: the worker's walk found no GPU-written dwords
    bool after_wait{};     ///< R.10 A1: follows a wait the worker found not yet met
    u8 wait_kind{};        ///< R.10 A1: worst such wait: 1 WaitRegMem (value-checked), 2 other
    u32 num_wait_pages{};  ///< R.10 A1: label pages of the met waits (not in read_pages)
    const u64* wait_pages{};
    u32 num_met_waits{};   ///< R.10 A1: the met waits, re-tested when their pages changed
    const SrtMetWait* met_waits{};
    const u8* check_bytes{}; ///< R.10 A1: the bytes the check runs read
    const PreparedDraw* dedup_prev{}; ///< R.10 A1: runs marked equal to this draw's
    u32 num_read_pages{};  ///< R.21: guest pages the walks read (0xFF: too many, untrusted)
    std::array<u64, 24> read_pages{};
    u32 ptr_page_mask{};   ///< R.10 A1 v8: bit i, read_pages[i] holds a table pointer read
    const Shader::SrtCheck* checks{}; ///< R.22: values the walks read, owned by the submission
    const Shader::SrtGuestRun* guest_runs{}; ///< R.10 A1 v9: guest ranges the walks read
    u32 num_guest_runs{};
    u32 num_checks{};
    GraphicsPipelineKey key{};
    u64 key_hash{};        ///< R.10 A1 v8: std::hash of key, computed by the worker
    u32 num_stages{};
    std::array<PreparedStage, MaxShaderStages> stages{};
    PreparedVertexInputs vertex{};
};

/// R.10 A1: true when every wait the worker found met before `draw` with its label on `page`
/// still holds.
bool SrtMetWaitsHold(const PreparedDraw& draw, u64 page);

class DrawPreparation {
public:
    explicit DrawPreparation(PipelineCache& pipeline_cache);
    ~DrawPreparation();

    DrawPreparation(const DrawPreparation&) = delete;
    DrawPreparation& operator=(const DrawPreparation&) = delete;

    [[nodiscard]] bool Enabled() const noexcept {
        return worker_count != 0;
    }

    struct Submission;
    /// Game submit thread, outside the queue lock: copies and scans a top-level graphics
    /// command buffer. Null when disabled.
    std::shared_ptr<Submission> Build(std::span<const u32> commands);
    /// Game submit thread, under the queue lock: hands it to the workers in submission order.
    void Enqueue(u64 seq, std::shared_ptr<Submission> submission);

    /// GPU thread: brackets the processing of submission `seq`. The first call hands the
    /// workers the exact register state and checksum they start replaying from.
    void BeginSubmission(u64 seq, const AmdGpu::Regs& regs, u64 reg_checksum);
    void EndSubmission();

    /// GPU thread: the prepared state for the next direct draw of the current submission.
    const PreparedDraw* NextDraw();

    /// GPU thread: counts whether a prepared draw was used; prints every 5 s (GOW3_FRAME_STATS).
    void Count(bool used);

    struct Submission {
        u64 seq{};
        std::vector<u32> commands;
        std::unique_ptr<PreparedDraw[]> draws;
        u32 num_draws{};
        std::vector<std::unique_ptr<u32[]>> flat_chunks; ///< from the claiming worker
        u64 start_checksum{}, end_checksum{}; ///< set by the scanner
        AmdGpu::RegDelta delta;                ///< registers written, values at the end
        std::atomic<bool> scanned{false};
        std::atomic<bool> claimed{false};
        std::atomic<bool> gpu_done{false};
    };

private:
    void ScannerLoop(std::stop_token stop);
    void WorkerLoop(std::stop_token stop, u32 index);
    void Collect();

    PipelineCache& pipeline_cache;
    std::unique_ptr<AmdGpu::Regs> initial_regs; ///< state at the start of `baseline_seq`
    u64 initial_checksum{};
    u64 baseline_seq{};
    /// State at the start of the oldest queued buffer (`tail_seq`): a worker whose position
    /// was collected restarts from here. Guarded by `mutex`.
    std::unique_ptr<AmdGpu::Regs> tail_regs;
    u64 tail_seq{};
    u64 tail_checksum{};
    u64 rebases = 0; ///< scanner restarts from the GPU thread state (GOW3_FRAME_STATS)
    bool baseline_ready{}; ///< guarded by `mutex`
    std::mutex mutex;
    std::condition_variable_any cv;
    std::deque<std::shared_ptr<Submission>> submissions; ///< ordered by seq
    std::atomic<u64> gpu_seq{0};
    std::shared_ptr<Submission> current;

public:
    /// gow3: the current submission (prepared draws), for the draw recording thread to keep
    /// it alive while it still records its draws.
    [[nodiscard]] std::shared_ptr<const void> KeepCurrent() const {
        return current;
    }

private:
    u32 current_draw = 0;
    u64 used = 0, unused = 0;
    u32 worker_count = 0; ///< fixed before the worker threads start
    std::jthread scanner;
    std::vector<std::jthread> workers;
};

} // namespace Vulkan
