// SPDX-License-Identifier: GPL-2.0-or-later
#include "gow3_write_log.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#undef near
#else
#include <ucontext.h>
#include <unistd.h>
#endif
#include <x86intrin.h>

// runtime_memory.c: the backing view of guest memory, past any page protection.
extern "C" int runtime_memory_region_backing(std::uintptr_t address, std::uintptr_t* start, std::uintptr_t* end,
                                             int* mapped, const unsigned char** backing);

namespace Gow3WriteLog {
namespace {
struct Entry {
    std::uint64_t address, size, first, tsc;
    std::uint32_t source, tid;
};
constexpr std::size_t Size = 1 << 20;
std::array<Entry, Size> ring;
std::atomic<std::uint64_t> head{0};
// Writes that contained the suspicious qword (see Note), with the exact address.
std::array<Entry, 256> hits;
std::atomic<std::uint64_t> hits_head{0};
constexpr std::uint64_t Pattern = 0x0000005300000000ull;
// uc3: GOW3_FAULT_WRITES=1, the last writes the GPU emulation made to guest memory past the page
// tracker (readbacks, leaf downloads, backing writes), kept apart and cheap enough for normal
// play, so a crash on corrupted guest data says which of them landed on it last.
constexpr std::size_t FaultSize = 1 << 16;
std::array<Entry, FaultSize> fault_ring;
std::atomic<std::uint64_t> fault_head{0};
constexpr std::uint32_t DownloadSource = 100;

void Push(std::array<Entry, Size>& r, std::atomic<std::uint64_t>& h, const Entry& e) {
    r[h.fetch_add(1, std::memory_order_relaxed) % r.size()] = e;
}
} // namespace

void Record(std::uint64_t address, const void* data, std::uint64_t size, Source source);

int Mode() {
    static const int mode = [] {
        const char* env = std::getenv("GOW3_WRITE_LOG");
        return env ? std::atoi(env) : 0;
    }();
    return mode;
}

bool Enabled() {
    return Mode() == 1;
}

bool FaultWrites() {
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_FAULT_WRITES");
        return env && env[0] == '1';
    }();
    return enabled;
}

void NoteFault(std::uint64_t address, const void* data, std::uint64_t size, std::uint32_t source) {
    static thread_local const std::uint32_t tid = static_cast<std::uint32_t>(gettid());
    Entry e{address, size, 0, __rdtsc(), source, tid};
    std::memcpy(&e.first, data, size < 8 ? size : 8);
    fault_ring[fault_head.fetch_add(1, std::memory_order_relaxed) % FaultSize] = e;
}

void NoteIntent(std::uint64_t address, const void* data, std::uint64_t size, Source source) {
    if (Mode() == 2) {
        Record(address, data, size, source);
    }
}

void Note(std::uint64_t address, const void* data, std::uint64_t size, Source source) {
    if (source == Backing && FaultWrites()) {
        NoteFault(address, data, size, Backing);
    }
    if (Enabled()) {
        Record(address, data, size, source);
    }
}

void NoteDownload(std::uint64_t address, const void* data, std::uint64_t size) {
    if (FaultWrites()) {
        NoteFault(address, data, size, DownloadSource);
    }
    if (!Enabled()) {
        return;
    }
    static std::atomic<int> shown{0};
    const std::uint64_t n = head.load(std::memory_order_relaxed);
    const std::uint64_t now = __rdtsc();
    for (std::uint64_t i = n; i-- > (n > 65536 ? n - 65536 : 0);) {
        const Entry& e = ring[i % Size];
        if ((e.source != Fence && e.source != WriteData) || e.address >= address + size ||
            e.address + e.size <= address) {
            continue;
        }
        if (shown.fetch_add(1, std::memory_order_relaxed) >= 50) {
            return;
        }
        std::uint64_t brings = 0;
        if (e.address >= address) {
            const std::uint64_t offset = e.address - address, left = size - offset;
            std::memcpy(&brings, static_cast<const unsigned char*>(data) + offset, left < 8 ? left : 8);
        }
        std::fprintf(stderr,
                     "Write log: readback %#llx +%llu overwrites %s %#llx +%llu (wrote %#llx, %.3f s ago, "
                     "tid %u) with %#llx\n",
                     (unsigned long long)address, (unsigned long long)size,
                     e.source == Fence ? "fence" : "WriteData", (unsigned long long)e.address,
                     (unsigned long long)e.size, (unsigned long long)e.first,
                     double(now - e.tsc) / 3.0e9, e.tid, (unsigned long long)brings);
    }
}

void Record(std::uint64_t address, const void* data, std::uint64_t size, Source source) {
    static thread_local const std::uint32_t tid = static_cast<std::uint32_t>(gettid());
    Entry e{address, size, 0, __rdtsc(), source, tid};
    std::memcpy(&e.first, data, size < 8 ? size : 8);
    Push(ring, head, e);
    // Only small writes are scanned: scanning downloads of megabytes delays them enough to hide
    // the race this log is for.
    if (size > 64) {
        return;
    }
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::uint64_t at = (8 - (address & 7)) & 7; at + 8 <= size; at += 8) {
        std::uint64_t v;
        std::memcpy(&v, bytes + at, 8);
        if (v == Pattern) {
            Entry hit = e;
            hit.address = address + at;
            hit.first = v;
            hits[hits_head.fetch_add(1, std::memory_order_relaxed) % hits.size()] = hit;
        }
    }
}
} // namespace Gow3WriteLog

namespace Gow3WriteLog {
namespace {
// uc3, GOW3_FAULT_WRITES=1: for each pointer register, the 0x40 bytes it points at (through the
// backing view) and the last logged writes into its 4 KiB page, newest first.
void DumpFaultWrites(const std::uint64_t* regs, const char* const* names, int count) {
    const std::uint64_t n = fault_head.load();
    const std::uint64_t now = __rdtsc();
    std::fprintf(stderr, "Fault writes: %llu writes logged (GOW3_FAULT_WRITES)\n", (unsigned long long)n);
    for (int r = 0; r < count; ++r) {
        const std::uint64_t at = regs[r];
        std::uintptr_t start = 0, end = 0;
        int mapped = 0;
        const unsigned char* backing = nullptr;
        if (at < 0x10000 || !runtime_memory_region_backing(at, &start, &end, &mapped, &backing) ||
            !backing || at < start || at + 0x40 > end) {
            continue;
        }
        std::fprintf(stderr, "Fault writes: %s=%#llx:", names[r], (unsigned long long)at);
        for (int q = 0; q < 8; ++q) {
            std::uint64_t v;
            std::memcpy(&v, backing + q * 8, 8);
            std::fprintf(stderr, " %016llx", (unsigned long long)v);
        }
        std::fprintf(stderr, "\n");
        const std::uint64_t page = at & ~0xfffull;
        int shown = 0;
        for (std::uint64_t i = n; i-- > (n > FaultSize ? n - FaultSize : 0) && shown < 16;) {
            const Entry& e = fault_ring[i % FaultSize];
            if (e.address >= page + 0x1000 || e.address + e.size <= page) {
                continue;
            }
            const bool hits = e.address < at + 0x40 && e.address + e.size > at;
            std::fprintf(stderr,
                         "Fault writes:   %s %#llx +%llu first %#llx tid %u, %.3f s before the fault%s\n",
                         e.source == DownloadSource ? "readback" : "backing write",
                         (unsigned long long)e.address, (unsigned long long)e.size,
                         (unsigned long long)e.first, e.tid, double(now - e.tsc) / 3.0e9,
                         hits ? " (COVERS IT)" : "");
            ++shown;
        }
    }
}
} // namespace
} // namespace Gow3WriteLog

extern "C" void gow3gpu_dump_guest_writes(void* ucontext) {
    using namespace Gow3WriteLog;
    if (Mode() == 0 && !FaultWrites()) {
        return;
    }
#ifdef _WIN32
    // The loader passes EXCEPTION_POINTERS (see signal_context.cpp).
    const CONTEXT* c = static_cast<const EXCEPTION_POINTERS*>(ucontext)->ContextRecord;
    const std::uint64_t regs[] = {c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->R14, c->R15};
    const std::uint64_t more[] = {c->Rdi, c->Rbx, c->Rax, c->R12, c->R13, c->R14, c->R15, c->Rsi};
#else
    const auto* uc = static_cast<const ucontext_t*>(ucontext);
    const auto* g = uc->uc_mcontext.gregs;
    const std::uint64_t regs[] = {std::uint64_t(g[REG_RAX]), std::uint64_t(g[REG_RBX]),
                                  std::uint64_t(g[REG_RCX]), std::uint64_t(g[REG_RDX]),
                                  std::uint64_t(g[REG_RSI]), std::uint64_t(g[REG_RDI]),
                                  std::uint64_t(g[REG_R14]), std::uint64_t(g[REG_R15])};
    const std::uint64_t more[] = {std::uint64_t(g[REG_RDI]), std::uint64_t(g[REG_RBX]),
                                  std::uint64_t(g[REG_RAX]), std::uint64_t(g[REG_R12]),
                                  std::uint64_t(g[REG_R13]), std::uint64_t(g[REG_R14]),
                                  std::uint64_t(g[REG_R15]), std::uint64_t(g[REG_RSI])};
#endif
    if (FaultWrites()) {
        const char* const more_names[] = {"rdi", "rbx", "rax", "r12", "r13", "r14", "r15", "rsi"};
        DumpFaultWrites(more, more_names, 8);
    }
    if (Mode() == 0) {
        return;
    }
    const char* names[] = {"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r14", "r15"};
    for (int i = 0; i < 8; ++i) {
        std::fprintf(stderr, "Write log: %s=%#llx\n", names[i], (unsigned long long)regs[i]);
    }
    const char* sources[] = {"backing",      "WriteData",           "fence",
                             "EOP (decoded)", "WriteData (decoded)", "EOS (decoded)"};
    const std::uint64_t now = __rdtsc();
    const auto print = [&](const Entry& e, const char* what) {
        std::fprintf(stderr,
                     "Write log: %s %s %#llx +%llu first %#llx tid %u, %.3f s before the fault\n",
                     what, e.source < 6 ? sources[e.source] : "?", (unsigned long long)e.address,
                     (unsigned long long)e.size, (unsigned long long)e.first, e.tid,
                     double(now - e.tsc) / 3.0e9);
    };
    const std::uint64_t nh = hits_head.load();
    for (std::uint64_t i = nh > hits.size() ? nh - hits.size() : 0; i < nh; ++i) {
        print(hits[i % hits.size()], "pattern");
    }
    // The block the guest read (rax) and all logged writes into it or its neighbours.
    const std::uint64_t block = regs[0];
    for (std::uint64_t at = block - 0x30; at < block + 0x60; at += 8) {
        std::uint64_t value = 0;
        std::memcpy(&value, reinterpret_cast<const void*>(at), 8);
        std::fprintf(stderr, "Write log: [%#llx] = %#llx\n", (unsigned long long)at,
                     (unsigned long long)value);
    }
    {
        const std::uint64_t n = head.load();
        int shown = 0;
        for (std::uint64_t i = n; i-- > (n > Size ? n - Size : 0) && shown < 200;) {
            const Entry& e = ring[i % Size];
            if (e.address + e.size > block - 0x30 && e.address < block + 0x60) {
                print(e, "in block");
                ++shown;
            }
        }
        shown = 0;
        for (std::uint64_t i = n; i-- > (n > Size ? n - Size : 0) && shown < 40;) {
            const Entry& e = ring[i % Size];
            if ((e.first & 0xffffffffull) == 0x53 || (e.first >> 32) == 0x53) {
                print(e, "value 0x53");
                ++shown;
            }
        }
    }
    // Writes that cover the chunk header the guest read (rax..rax+0x40) or the registers.
    const std::uint64_t n = head.load();
    int shown = 0;
    for (std::uint64_t i = n; i-- > (n > Size ? n - Size : 0) && shown < 64;) {
        const Entry& e = ring[i % Size];
        bool near = false;
        for (const auto r : regs) {
            near |= r + 0x1000 > e.address && r < e.address + e.size + 0x1000;
        }
        if (near) {
            print(e, "near");
            ++shown;
        }
    }
    // uc3: the labels the GPU emulation wrote lately (fences, WriteData), as guest memory holds
    // them now, read through the backing view (a page may be read-protected): a label that no
    // longer holds what was written was overwritten, and the writes that landed there say by
    // whom. The most recent write per address counts.
    {
        struct Label { std::uint64_t address, size, first, tsc; std::uint32_t source; };
        Label labels[96];
        int count = 0;
        for (std::uint64_t i = n; i-- > (n > 20000 ? n - 20000 : 0) && count < 96;) {
            const Entry& e = ring[i % Size];
            if (e.source != Fence && e.source != WriteData) {
                continue;
            }
            bool seen = false;
            for (int k = 0; k < count && !seen; ++k) {
                seen = labels[k].address == e.address;
            }
            if (!seen) {
                labels[count++] = {e.address, e.size, e.first, e.tsc, e.source};
            }
        }
        int changed = 0;
        for (int k = 0; k < count; ++k) {
            const Label& l = labels[k];
            std::uintptr_t start = 0, end = 0;
            int mapped = 0;
            const unsigned char* backing = nullptr;
            std::uint64_t holds = 0;
            if (runtime_memory_region_backing(l.address, &start, &end, &mapped, &backing) && backing &&
                l.address + 8 <= end) {
                std::memcpy(&holds, backing, 8);
            }
            const bool differs = (l.size >= 8 ? holds : holds & 0xffffffffull) !=
                                 (l.size >= 8 ? l.first : l.first & 0xffffffffull);
            if (differs || k < 12) {
                std::fprintf(stderr,
                             "Write log: label %#llx +%llu: %s wrote %#llx %.3f s before the fault; now holds %#llx%s\n",
                             (unsigned long long)l.address, (unsigned long long)l.size,
                             l.source == Fence ? "fence" : "WriteData", (unsigned long long)l.first,
                             double(now - l.tsc) / 3.0e9, (unsigned long long)holds,
                             differs ? " (CHANGED)" : "");
            }
            if (!differs || changed++ >= 16) {
                continue;
            }
            int listed = 0;
            for (std::uint64_t i = n; i-- > (n > Size ? n - Size : 0) && listed < 8;) {
                const Entry& e = ring[i % Size];
                if (e.address < l.address + 8 && e.address + e.size > l.address) {
                    print(e, "  over it");
                    ++listed;
                }
            }
        }
        std::fprintf(stderr, "Write log: %d label addresses checked, %d changed\n", count, changed);
    }
    std::fprintf(stderr, "Write log: %llu writes logged\n", (unsigned long long)n);
}
