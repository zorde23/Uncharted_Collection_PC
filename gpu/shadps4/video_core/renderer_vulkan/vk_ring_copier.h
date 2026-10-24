// SPDX-License-Identifier: GPL-2.0-or-later
// gow3 R.9.12 (GOW3_RING_COPY_THREAD=1): stage A (the GPU command thread) spends about a
// quarter of its time copying constants from guest memory into the constant ring
// (vk_constant_ring.h, ~2 GB/s in the streets). With this on, stage A only queues each copy
// here and a copy thread runs them in order. Before stage B runs a packet it waits until the
// copies queued before that packet are done, so stage B and the GPU see the same bytes as
// before. Guest memory writes stage A makes itself (MemSemaphore signal, DumpConstRam) first
// wait for every queued copy (Rasterizer::WaitRingCopies); writes handed to stage B
// (RunInOrder) run after their packet's wait, and other packets drain stage B first.
//
// ucndc R.12 A2+A3 (Fable, GOW3_RING_PREP=1, needs GOW3_RING_COPY_STEAL=1): stage A no longer
// checks, deduplicates or allocates the constants of a packet: it queues the packet on an
// ordered prepare queue and this thread does that work (Rasterizer::PrepareRingPacket), queuing
// the copies on the stealable queue. Stage B waits for the prepare, then for the copies.
// R.12b (Fable): prepares are claimed in order like the copies, so a thread waiting for one
// (stage B right after a drain, stage A with a full queue) runs it itself instead of waking the
// copy thread and waiting; job n runs only once job n - 1 is done (prepared == n), which also
// hands the dedup table and the constant ring from one thread to the next.

#pragma once

#include <algorithm>
#include <array>
#include <string>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <x86intrin.h>

#include "common/thread.h"
#include "common/types.h"

namespace Vulkan {

class RingCopier {
public:
    /// Copies `size` bytes of guest memory at `address` (or from `src` when not null) to `dst`
    /// (ring offset `offset`).
    using CopyFn = void (*)(void* context, VAddr address, const void* src, u8* dst, u64 size,
                            u64 offset);

    /// R.12: a job of the prepare queue, run in order by the copy thread.
    enum class PrepKind : u32 { Packet, NoteWrite, DedupClear };
    struct PrepJob {
        PrepKind kind;
        u8* packet;    ///< Packet: the packet in the draw pipe
        VAddr address; ///< NoteWrite
        u64 size;
    };
    using PrepFn = void (*)(void* context, const PrepJob& job);

    RingCopier(CopyFn fn_, void* context_) : fn{fn_}, context{context_} {
        jobs = std::make_unique<Job[]>(Capacity);
        // R.9.14: GOW3_RING_COPY_THREADS=N (1-4) splits the copies over N threads, copy j on
        // thread j % N: a packet waits for less (each thread copies its share in order).
        if (const char* env = std::getenv("GOW3_RING_COPY_THREADS")) {
            num_threads = std::clamp(std::atoi(env), 1, int(MaxThreads));
        }
        // ucndc R.11 item 2 (Fable): GOW3_RING_COPY_STEAL=1 - copies are claimed from a shared
        // queue; whoever waits for a copy (stage B, stage A) runs it itself, and the copy thread
        // sleeps until GOW3_RING_COPY_WAKE_N copies (default 256) piled up. One copy thread only.
        if (const char* env = std::getenv("GOW3_RING_COPY_STEAL"); env && env[0] == '1') {
            if (num_threads == 1) {
                steal = true;
                finished = std::make_unique<std::atomic<u64>[]>(Capacity);
                if (const char* n = std::getenv("GOW3_RING_COPY_WAKE_N"); n && n[0]) {
                    wake_n = std::max<u64>(1, std::strtoull(n, nullptr, 10));
                }
                std::printf("Ring copy: waiters run the copies they wait for; the copy thread "
                            "wakes every %llu copies\n",
                            static_cast<unsigned long long>(wake_n));
            } else {
                std::printf("Ring copy: GOW3_RING_COPY_STEAL needs one copy thread, ignored\n");
            }
        }
        for (u32 i = 0; i < num_threads; ++i) {
            threads[i] = std::jthread([this, i](std::stop_token stop) {
                if (steal) {
                    RunSteal(stop);
                } else {
                    Run(stop, i);
                }
            });
        }
    }

    ~RingCopier() {
        for (u32 i = 0; i < num_threads; ++i) {
            threads[i].request_stop();
        }
        wake.fetch_add(1, std::memory_order_seq_cst);
        wake.notify_all();
    }

    static bool Wanted() {
        static const bool on = [] {
            const char* env = std::getenv("GOW3_RING_COPY_THREAD");
            return env && env[0] == '1';
        }();
        return on;
    }

    /// Stage A (the copy thread with GOW3_RING_PREP): queues a copy.
    void Push(VAddr address, u8* dst, u64 size, u64 offset, const void* src = nullptr) {
        if (steal) {
            if (head >= Capacity) {
                WaitDone(head - Capacity + 1);
            }
            jobs[head % Capacity] = {address, src, dst, size, offset};
            ++head;
            enqueued.store(head, std::memory_order_seq_cst);
            if (sleeping.load(std::memory_order_seq_cst) != 0 &&
                head - sleep_at.load(std::memory_order_seq_cst) >= wake_n &&
                !woken.exchange(true, std::memory_order_seq_cst)) {
                // One wake per sleep: later pushes see woken until the thread sleeps again.
                notifies.fetch_add(1, std::memory_order_relaxed);
                wake.fetch_add(1, std::memory_order_seq_cst);
                wake.notify_all();
            }
            return;
        }
        while (head >= Capacity && !IsDone(head - Capacity + 1)) {
            __builtin_ia32_pause();
        }
        jobs[head % Capacity] = {address, src, dst, size, offset};
        ++head;
        enqueued.store(head, std::memory_order_seq_cst);
        if (sleeping.load(std::memory_order_seq_cst) != 0) {
            wake.fetch_add(1, std::memory_order_seq_cst);
            wake.notify_all();
        }
    }

    /// The number of copies queued so far (a packet's sequence number).
    [[nodiscard]] u64 Enqueued() const noexcept {
        return enqueued.load(std::memory_order_acquire);
    }

    /// True once the first `seq` queued copies are done.
    [[nodiscard]] bool IsDone(u64 seq) const noexcept {
        if (steal) {
            return done_prefix.load(std::memory_order_acquire) >= seq;
        }
        for (u32 i = 0; i < num_threads; ++i) {
            // Copies below seq that thread i runs: i, i + N, ...
            const u64 count = seq > i ? (seq - i + num_threads - 1) / num_threads : 0;
            if (done[i].value.load(std::memory_order_acquire) < count) {
                return false;
            }
        }
        return true;
    }

    /// Any thread: waits until the first `seq` queued copies are done.
    void WaitDone(u64 seq, bool stage_b = false) {
        if (IsDone(seq)) {
            return;
        }
        const u64 start = __rdtsc();
        if (steal) {
            // Run the copies this wait needs; when they are all claimed, wait for the others.
            u64 copied = 0;
            u64 copy_cycles = 0;
            for (u32 spins = 0; !IsDone(seq);) {
                const u64 c0 = __rdtsc();
                if (const u64 n = Claim(seq); n != NoJob) {
                    RunJob(n);
                    copy_cycles += __rdtsc() - c0;
                    ++copied;
                    continue;
                }
                if (spins++ < 4096) {
                    __builtin_ia32_pause();
                } else {
                    std::this_thread::yield();
                }
            }
            (stage_b ? b_copies : a_copies).fetch_add(copied, std::memory_order_relaxed);
            (stage_b ? b_copy_cycles : a_copy_cycles)
                .fetch_add(copy_cycles, std::memory_order_relaxed);
            (stage_b ? b_wait_cycles : wait_cycles)
                .fetch_add(__rdtsc() - start, std::memory_order_relaxed);
            (stage_b ? b_waits : waits).fetch_add(1, std::memory_order_relaxed);
            return;
        }
        for (u32 spins = 0; !IsDone(seq); ++spins) {
            if (spins < 4096) {
                __builtin_ia32_pause();
            } else {
                std::this_thread::yield();
            }
        }
        (stage_b ? b_wait_cycles : wait_cycles).fetch_add(__rdtsc() - start, std::memory_order_relaxed);
        (stage_b ? b_waits : waits).fetch_add(1, std::memory_order_relaxed);
    }

    /// Any thread: waits for every copy queued so far (with GOW3_RING_PREP, for every packet
    /// queued so far to be prepared first).
    void WaitAll() {
        if (PrepOn()) {
            WaitPrepared(prep_enqueued.load(std::memory_order_acquire));
        }
        WaitDone(Enqueued());
    }

    /// R.12: turns the prepare queue on (before the first packet). Needs the STEAL mode.
    bool EnablePrep(PrepFn fn_) {
        if (!steal) {
            return false;
        }
        prep_jobs = std::make_unique<PrepJob[]>(PrepCapacity);
        if (const char* n = std::getenv("GOW3_RING_PREP_WAKE_N"); n && n[0]) {
            prep_wake_n = std::max<u64>(1, std::strtoull(n, nullptr, 10));
        }
        if (const char* n = std::getenv("GOW3_RING_PREP_SPIN_US"); n && n[0]) {
            prep_spin_us = static_cast<u32>(std::strtoul(n, nullptr, 10));
        }
        prep_fn = fn_;
        prep_on.store(true, std::memory_order_seq_cst);
        std::printf("Ring prep: the copy thread prepares the constants of each packet (R.12); it "
                    "spins %u us before sleeping and stage A wakes it at %llu packets\n",
                    prep_spin_us, static_cast<unsigned long long>(prep_wake_n));
        return true;
    }

    [[nodiscard]] bool PrepOn() const noexcept {
        return prep_on.load(std::memory_order_acquire);
    }

    /// R.12, stage A: queues a prepare job.
    void PushPrep(const PrepJob& job) {
        if (prep_head - prepared.load(std::memory_order_acquire) >= PrepCapacity) {
            WaitPrepared(prep_head - PrepCapacity + 1);
        }
        prep_jobs[prep_head % PrepCapacity] = job;
        ++prep_head;
        prep_enqueued.store(prep_head, std::memory_order_seq_cst);
        if (sleeping.load(std::memory_order_seq_cst) != 0 &&
            prep_head - prep_claim.load(std::memory_order_seq_cst) >= prep_wake_n &&
            !woken.exchange(true, std::memory_order_seq_cst)) {
            notifies.fetch_add(1, std::memory_order_relaxed);
            wake.fetch_add(1, std::memory_order_seq_cst);
            wake.notify_all();
        }
    }

    /// R.12, stage A: prepare jobs queued so far (a packet's prepare sequence number).
    [[nodiscard]] u64 PrepEnqueued() const noexcept {
        return prep_head;
    }

    /// R.12, any thread: waits until the first `seq` prepare jobs ran, waking the copy thread
    /// if it sleeps (the waiter count keeps it from going to sleep meanwhile).
    void WaitPrepared(u64 seq, bool stage_b = false) {
        if (prepared.load(std::memory_order_acquire) >= seq) {
            return;
        }
        const u64 start = __rdtsc();
        prep_waiters.fetch_add(1, std::memory_order_seq_cst);
        for (u32 spins = 0; prepared.load(std::memory_order_acquire) < seq;) {
            // R.12b: run the next unclaimed prepare below seq; when all are claimed, the
            // threads running them finish them in order.
            if (RunPrep(seq)) {
                (stage_b ? b_prep_runs : a_prep_runs).fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (spins++ < 4096) {
                __builtin_ia32_pause();
            } else {
                std::this_thread::yield();
            }
        }
        prep_waiters.fetch_sub(1, std::memory_order_seq_cst);
        (stage_b ? b_wait_cycles : wait_cycles).fetch_add(__rdtsc() - start, std::memory_order_relaxed);
        (stage_b ? b_prep_waits : a_prep_waits).fetch_add(1, std::memory_order_relaxed);
    }

    /// Statistics: cycles spent waiting for copies (and how many waits), cycles copying.
    std::atomic<u64> wait_cycles{0}, waits{0}, busy_cycles{0};
    std::atomic<u64> b_wait_cycles{0}, b_waits{0}; ///< stage B's share
    /// GOW3_RING_COPY_STEAL: copies run by the copy thread / stage B / the others, the cycles
    /// stage B and the others spent copying (part of their wait), sleeps and wakes.
    std::atomic<u64> thread_copies{0}, b_copies{0}, a_copies{0};
    std::atomic<u64> b_copy_cycles{0}, a_copy_cycles{0}, sleeps{0}, notifies{0};
    /// R.12: cycles the copy thread spent preparing, packets prepared, the prepare backlog
    /// summed at each drain (and the drains), waits for a prepare by stage B and the others.
    std::atomic<u64> prep_cycles{0}, prep_packets{0}, prep_backlog{0}, prep_drains{0};
    std::atomic<u64> b_prep_waits{0}, a_prep_waits{0};
    /// R.12b: prepare jobs run by stage B / the others while waiting.
    std::atomic<u64> b_prep_runs{0}, a_prep_runs{0};

private:
    struct Job {
        VAddr address;
        const void* src; ///< R.12: memcpy source (the flat), null for guest memory
        u8* dst;
        u64 size;
        u64 offset;
    };
    static constexpr u64 Capacity = 1ull << 16;
    static constexpr u64 PrepCapacity = 1ull << 15;
    static constexpr u32 MaxThreads = 4;

    static u32 SpinMicros() {
        static const u32 micros = [] {
            const char* env = std::getenv("GOW3_RING_COPY_SPIN_US");
            const u32 value = env && env[0] ? static_cast<u32>(std::strtoul(env, nullptr, 10)) : 200;
            if (value != 200) {
                std::printf("Ring copy: the copy threads spin %u us before sleeping\n", value);
            }
            return value;
        }();
        return micros;
    }

    static constexpr u64 NoJob = ~0ull;

    /// GOW3_RING_COPY_STEAL: claims the next unclaimed copy below `limit` (and below what was
    /// queued), or returns NoJob.
    u64 Claim(u64 limit) {
        u64 n = claim.load(std::memory_order_seq_cst);
        while (true) {
            const u64 end = std::min(limit, enqueued.load(std::memory_order_seq_cst));
            if (n >= end) {
                return NoJob;
            }
            if (claim.compare_exchange_weak(n, n + 1, std::memory_order_seq_cst)) {
                return n;
            }
        }
    }

    /// R.12b: claims the next unclaimed prepare job below `limit` (and below what was queued),
    /// waits until the jobs before it are done and runs it. False when there is none to claim.
    bool RunPrep(u64 limit) {
        u64 n = prep_claim.load(std::memory_order_seq_cst);
        while (true) {
            const u64 end = std::min(limit, prep_enqueued.load(std::memory_order_seq_cst));
            if (n >= end) {
                return false;
            }
            if (prep_claim.compare_exchange_weak(n, n + 1, std::memory_order_seq_cst)) {
                break;
            }
        }
        while (prepared.load(std::memory_order_seq_cst) != n) {
            __builtin_ia32_pause();
        }
        const u64 c0 = __rdtsc();
        const PrepJob& job = prep_jobs[n % PrepCapacity];
        const bool packet = job.kind == PrepKind::Packet;
        prep_fn(context, job);
        prepared.store(n + 1, std::memory_order_seq_cst);
        prep_packets.fetch_add(packet, std::memory_order_relaxed);
        prep_cycles.fetch_add(__rdtsc() - c0, std::memory_order_relaxed);
        return true;
    }

    /// GOW3_RING_COPY_STEAL: runs claimed copy `n`, then moves done_prefix past every copy
    /// finished in order. A copy finished out of order is picked up either by the thread that
    /// moves the prefix up to it or by its own thread re-reading the prefix (both seq_cst).
    void RunJob(u64 n) {
        const Job& job = jobs[n % Capacity];
        fn(context, job.address, job.src, job.dst, job.size, job.offset);
        finished[n % Capacity].store(n + 1, std::memory_order_seq_cst);
        u64 p = done_prefix.load(std::memory_order_seq_cst);
        while (finished[p % Capacity].load(std::memory_order_seq_cst) == p + 1) {
            if (done_prefix.compare_exchange_weak(p, p + 1, std::memory_order_seq_cst)) {
                ++p;
            }
        }
    }

    /// GOW3_RING_COPY_STEAL: one log line every ~5 s (from the copy thread, when it is awake):
    /// who ran the copies; the "waited for it" of the draw pipe line includes the copying.
    std::chrono::steady_clock::time_point ReportSteal() {
        const auto now = std::chrono::steady_clock::now();
        const u64 tsc = __rdtsc();
        const double seconds = std::chrono::duration<double>(now - report_last).count();
        const double cycles = double(tsc - report_tsc);
        const u64 t = thread_copies.load(std::memory_order_relaxed);
        const u64 bc = b_copies.load(std::memory_order_relaxed);
        const u64 ac = a_copies.load(std::memory_order_relaxed);
        const u64 bcyc = b_copy_cycles.load(std::memory_order_relaxed);
        const u64 acyc = a_copy_cycles.load(std::memory_order_relaxed);
        const u64 tcyc = busy_cycles.load(std::memory_order_relaxed);
        const u64 sl = sleeps.load(std::memory_order_relaxed);
        const u64 no = notifies.load(std::memory_order_relaxed);
        if (report_tsc != 0 && seconds > 0.0) {
            std::printf("Ring copy steal: copies/s by the thread %.0f, by stage B %.0f, by others "
                        "%.0f; thread copying %.1f%%, stage B copying %.1f%%, others copying "
                        "%.1f%%; sleeps %.0f/s, wakes %.0f/s\n",
                        (t - r_thread) / seconds, (bc - r_b) / seconds, (ac - r_a) / seconds,
                        100.0 * (tcyc - r_tcyc) / cycles, 100.0 * (bcyc - r_bcyc) / cycles,
                        100.0 * (acyc - r_acyc) / cycles, (sl - r_sleeps) / seconds,
                        (no - r_notifies) / seconds);
        }
        r_thread = t;
        r_b = bc;
        r_a = ac;
        r_bcyc = bcyc;
        r_acyc = acyc;
        r_tcyc = tcyc;
        r_sleeps = sl;
        r_notifies = no;
        report_last = now;
        report_tsc = tsc;
        return now + std::chrono::seconds(5);
    }

    /// GOW3_RING_COPY_STEAL: the copy thread drains in the background and sleeps for real.
    void RunSteal(std::stop_token stop) {
        Common::SetCurrentThreadName("gow3:RingCopy0");
        // The waiters run the copies they need, so by default the thread sleeps right after draining.
        // R.12: with the prepare queue it spins GOW3_RING_PREP_SPIN_US (20) first: packets come
        // every few microseconds inside a frame.
        const bool copy_spin = std::getenv("GOW3_RING_COPY_SPIN_US") != nullptr;
        auto report_at = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!stop.stop_requested()) {
            if (std::chrono::steady_clock::now() >= report_at) {
                report_at = ReportSteal();
            }
            if (PrepOn()) {
                // R.12c: strict order per packet. One prepare, then the copies it (and the ones
                // before it) queued, so stage B finds packet n prepared and copied before the
                // thread moves on to n+1.
                const u64 p = prep_claim.load(std::memory_order_seq_cst);
                const u64 end = prep_enqueued.load(std::memory_order_acquire);
                if (p < end) {
                    const u64 c0 = __rdtsc();
                    prep_backlog.fetch_add(end - p, std::memory_order_relaxed);
                    prep_drains.fetch_add(1, std::memory_order_relaxed);
                    if (RunPrep(NoJob)) {
                        const u64 upto = Enqueued();
                        u64 count = 0;
                        for (u64 m; (m = Claim(upto)) != NoJob; ++count) {
                            RunJob(m);
                        }
                        thread_copies.fetch_add(count, std::memory_order_relaxed);
                    }
                    busy_cycles.fetch_add(__rdtsc() - c0, std::memory_order_relaxed);
                    continue;
                }
            }
            const u64 start = __rdtsc();
            if (const u64 n = Claim(NoJob); n != NoJob) {
                RunJob(n);
                u64 count = 1;
                // R.12: back to the prepare queue as soon as a packet waits there.
                for (u64 m; !PrepPending() && (m = Claim(NoJob)) != NoJob; ++count) {
                    RunJob(m);
                }
                thread_copies.fetch_add(count, std::memory_order_relaxed);
                busy_cycles.fetch_add(__rdtsc() - start, std::memory_order_relaxed);
                continue;
            }
            // EnablePrep comes after this thread started: read the spin here.
            const u32 spin_us = copy_spin ? SpinMicros() : PrepOn() ? prep_spin_us : 0;
            const auto spin_until =
                std::chrono::steady_clock::now() + std::chrono::microseconds(spin_us);
            bool found = false;
            for (u32 spins = 1; spin_us != 0 && !stop.stop_requested(); ++spins) {
                __builtin_ia32_pause();
                if (claim.load(std::memory_order_seq_cst) <
                        enqueued.load(std::memory_order_seq_cst) ||
                    PrepPending()) {
                    found = true;
                    break;
                }
                if (!(spins & 31) && std::chrono::steady_clock::now() >= spin_until) {
                    break;
                }
            }
            if (found || stop.stop_requested()) {
                continue;
            }
            const u32 seen = wake.load(std::memory_order_seq_cst);
            sleep_at.store(claim.load(std::memory_order_seq_cst), std::memory_order_seq_cst);
            woken.store(false, std::memory_order_seq_cst);
            sleeping.fetch_add(1, std::memory_order_seq_cst);
            // Unclaimed copies are left to the waiters until wake_n of them pile up. R.12:
            // prepares wait until prep_wake_n of them pile up or someone waits for one.
            if (enqueued.load(std::memory_order_seq_cst) - sleep_at.load(std::memory_order_seq_cst) <
                    wake_n &&
                (!PrepOn() ||
                 (prep_enqueued.load(std::memory_order_seq_cst) -
                          prep_claim.load(std::memory_order_seq_cst) <
                      prep_wake_n &&
                  prep_waiters.load(std::memory_order_seq_cst) == 0)) &&
                !stop.stop_requested()) {
                sleeps.fetch_add(1, std::memory_order_relaxed);
                wake.wait(seen, std::memory_order_seq_cst);
            }
            sleeping.fetch_sub(1, std::memory_order_seq_cst);
        }
    }

    /// R.12: a prepare job is waiting.
    bool PrepPending() const noexcept {
        return PrepOn() && prep_claim.load(std::memory_order_relaxed) <
                               prep_enqueued.load(std::memory_order_acquire);
    }

    /// Thread `index` runs copies index, index + N, ...
    void Run(std::stop_token stop, u32 index) {
        Common::SetCurrentThreadName(("gow3:RingCopy" + std::to_string(index)).c_str());
        const u64 step = num_threads;
        u64 at = index;
        u64 count = 0;
        while (true) {
            u64 available = enqueued.load(std::memory_order_acquire);
            if (available <= at) {
                // Copies come every few microseconds while a frame is decoded: spin, and
                // sleep only after a longer pause (as the draw pipe does). ucndc R.11 (Fable):
                // GOW3_RING_COPY_SPIN_US sets that pause (default 200 us); with copies queued
                // about every 0.6 us the thread rarely sleeps inside a frame and holds a core.
                const auto spin_until =
                    std::chrono::steady_clock::now() + std::chrono::microseconds(SpinMicros());
                const u32 check = SpinMicros() < 50 ? 31 : 255; // short spins check the clock sooner
                for (u32 spins = 1; available <= at; ++spins) {
                    if (stop.stop_requested()) {
                        return;
                    }
                    __builtin_ia32_pause();
                    if (!(spins & check) && std::chrono::steady_clock::now() >= spin_until) {
                        const u32 seen = wake.load(std::memory_order_seq_cst);
                        sleeping.fetch_add(1, std::memory_order_seq_cst);
                        if (enqueued.load(std::memory_order_seq_cst) <= at &&
                            !stop.stop_requested()) {
                            wake.wait(seen, std::memory_order_seq_cst);
                        }
                        sleeping.fetch_sub(1, std::memory_order_relaxed);
                    }
                    available = enqueued.load(std::memory_order_acquire);
                }
            }
            const u64 start = __rdtsc();
            while (at < available) {
                const Job& job = jobs[at % Capacity];
                fn(context, job.address, job.src, job.dst, job.size, job.offset);
                at += step;
                done[index].value.store(++count, std::memory_order_release);
            }
            busy_cycles.fetch_add(__rdtsc() - start, std::memory_order_relaxed);
        }
    }

    CopyFn fn;
    void* context;
    std::unique_ptr<Job[]> jobs;
    u64 head = 0; ///< stage A
    u32 num_threads = 1;
    alignas(64) std::atomic<u64> enqueued{0};
    struct alignas(64) Done {
        std::atomic<u64> value{0}; ///< copies done by one thread
    };
    std::array<Done, MaxThreads> done{};
    alignas(64) std::atomic<u32> wake{0};
    std::atomic<u32> sleeping{0};
    // GOW3_RING_COPY_STEAL
    bool steal = false;
    u64 wake_n = 256;
    std::unique_ptr<std::atomic<u64>[]> finished; ///< finished[n % Capacity] = n + 1 when done
    alignas(64) std::atomic<u64> claim{0};
    alignas(64) std::atomic<u64> done_prefix{0};
    alignas(64) std::atomic<u64> sleep_at{0};
    std::atomic<bool> woken{false}; ///< a producer already woke the sleeping copy thread
    // R.12 prepare queue: stage A pushes, the copy thread runs in order
    std::atomic<bool> prep_on{false};
    PrepFn prep_fn = nullptr;
    std::unique_ptr<PrepJob[]> prep_jobs;
    u64 prep_head = 0; ///< stage A
    u64 prep_wake_n = 1; ///< R.12b: wake at the first prepare (one wake per sleep)
    u32 prep_spin_us = 20;
    alignas(64) std::atomic<u64> prep_enqueued{0};
    alignas(64) std::atomic<u64> prepared{0};
    alignas(64) std::atomic<u64> prep_claim{0}; ///< R.12b: next prepare job to claim
    alignas(64) std::atomic<u32> prep_waiters{0};
    // ReportSteal state (copy thread only)
    std::chrono::steady_clock::time_point report_last{};
    u64 report_tsc = 0;
    u64 r_thread = 0, r_b = 0, r_a = 0, r_bcyc = 0, r_acyc = 0, r_tcyc = 0, r_sleeps = 0,
        r_notifies = 0;
    std::array<std::jthread, MaxThreads> threads;
};

} // namespace Vulkan
