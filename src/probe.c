/* Minimal x86-64 native loader experiment. Not a PS4 emulator or game port. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "runtime.h"
#include "orb_hook.h"
#include "cheat_hook.h"
#include "actor_hook.h"
#include "damage_hook.h"
#include "gpu/gow3gpu.h"
#if !defined(__x86_64__) || !defined(__GNUC__)
#error This prototype requires x86-64 GCC or Clang (including MinGW).
#endif
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <malloc.h>
#include <unistd.h>
#include <signal.h>
#include <ucontext.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#endif

typedef struct { uint64_t address, size, flags; } Segment;
typedef struct { uint64_t target, kind, value, addend; } Reloc;
static char (*names)[128];
static uint64_t import_count;
static unsigned char *image;
static size_t page_size;
typedef struct { uint64_t base, size, init, tls_address, tls_memsz, tls_filesz, tls_module; } LinkedModule;
static LinkedModule modules[16];
static uint64_t module_count;
static int entered_game;
static int gpu_enabled;
int vulkan_smoke(void);

static void fail(const char *message) { fprintf(stderr, "ERROR: %s\n", message); exit(1); }
static uint64_t read64(FILE *f) {
    unsigned char b[8];
    if (fread(b, 1, 8, f) != 8) fail("truncated boot file");
    uint64_t n = 0;
    for (int i = 7; i >= 0; --i) n = (n << 8) | b[i];
    return n;
}
static size_t round_page(size_t size) { return (size + page_size - 1) & ~(page_size - 1); }
static void *allocate(size_t size) {
    void *low=runtime_low_map(size,PROT_READ|PROT_WRITE);
    if (low) return low;
#ifdef _WIN32
    void *p = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) fail("VirtualAlloc failed");
#else
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) fail("mmap failed");
#endif
    return p;
}
static void protect(void *p, size_t size, unsigned flags) {
#ifdef _WIN32
    DWORD old, mode = PAGE_NOACCESS;
    if (flags & 1) mode = (flags & 2) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    else if (flags & 2) mode = PAGE_READWRITE;
    else if (flags & 4) mode = PAGE_READONLY;
    if (!VirtualProtect(p, size, mode, &old)) fail("VirtualProtect failed");
    FlushInstructionCache(GetCurrentProcess(), p, size);
#else
    int mode = ((flags & 4) ? PROT_READ : 0) | ((flags & 2) ? PROT_WRITE : 0) | ((flags & 1) ? PROT_EXEC : 0);
    if (mprotect(p, size, mode)) fail("mprotect failed");
#endif
}
static ABI __attribute__((noreturn)) void unresolved(uint32_t id, uintptr_t argument) {
    if (id >= import_count) fail("bad import trap index");
    printf("STOP: first unsupported PS4 import: %s (index %u)\n", names[id], id);
    printf("API: %s\n", runtime_import_name(names[id]));
    printf("Caller return offset: 0x%" PRIxPTR "; first argument: 0x%" PRIxPTR "\n",
           (uintptr_t)__builtin_return_address(0) - (uintptr_t)image, argument);
    uintptr_t caller=(uintptr_t)__builtin_return_address(0)-(uintptr_t)image;
    for (uint64_t m=0;m<module_count;++m)
        if (caller>=modules[m].base && caller-modules[m].base<modules[m].size)
            printf("Caller in linked module %" PRIu64 " (%s): +0x%" PRIxPTR "\n",m,m==0 ? "libc.prx" : "system module",caller-modules[m].base);
    runtime_report();
    puts(entered_game ? "Original guest entry instructions executed; game initialization is incomplete." :
                        "Native libc initialization is incomplete; game entry has not run.");
    fflush(NULL);
    _exit(20); /* no destructors: GPU, audio and guest threads are still running */
}
#ifndef _WIN32
/* enter_on_stack(entry, arg0, arg1, stack_top): call entry(arg0,arg1) on a new stack. */
void enter_on_stack(void *entry, void *arg0, void *arg1, void *top);
__asm__(".text\n.globl enter_on_stack\nenter_on_stack:\n"
        " push %rbp\n mov %rsp,%rbp\n and $-16,%rcx\n mov %rcx,%rsp\n"
        " mov %rdi,%rax\n mov %rsi,%rdi\n mov %rdx,%rsi\n call *%rax\n"
        " mov %rbp,%rsp\n pop %rbp\n ret\n");
#endif
static ABI void guest_exit(void) { puts("Runtime: process finalizer callback reached"); }
#ifdef _WIN32
static void describe_address(char *out, size_t size, uintptr_t address) {
    HMODULE module=NULL;
    char name[MAX_PATH]="?";
    if (image && address-(uintptr_t)image<0x20000000) { snprintf(out,size,"guest offset 0x%" PRIxPTR,address-(uintptr_t)image); return; }
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)address,&module) && module) {
        GetModuleFileNameA(module,name,sizeof(name));
        const char *base=strrchr(name,'\\');
        snprintf(out,size,"%s+0x%" PRIxPTR,base ? base+1 : name,address-(uintptr_t)module);
        return;
    }
    snprintf(out,size,"%p",(void *)address);
}
/* uc3 GOW3_CB_WATCH=1 (diagnostic, off; Opus's design for the Uncharted 1.02 crash in the
 * command buffer writer 0x63e260: a job's command buffer object comes back with its out-of-space
 * callback (+0x18) zeroed and its user data (+0x20) pointing at a stack local, and the next full
 * buffer calls address 0). The objects live in frame-allocator memory that is reused within a
 * frame, so a watch on fixed addresses only sees that reuse; this one follows the objects.
 * Two int3s, kept for the whole session:
 * - birth, 0x6422b4 (`movq $0,0x30(%rbx)`, the handler does it), right after the init stores
 *   the callback and user data: the initializing thread watches the new object;
 * - use, 0x501e9d (`lea rdi,[rbx+0x2d0]`, the handler does it), where a job hands the object to
 *   the writer: the using thread watches it.
 * Each thread's DR0/DR1 watch +0x18/+0x20 of its current object, DR2/DR3 of its previous one.
 * Every watched write goes silently into that thread's ring (instruction, value, tsc, the
 * guest's frame and the end-of-pipe flips reached), with a short guest backtrace when +0x18
 * becomes 0; births and uses go into a second ring. Nothing is printed while playing except a
 * line a minute; the rings are printed at a fatal fault. */
enum { CBW_THREADS = 64, CBW_HITS = 64, CBW_OBJECTS = 16, CBW_BT = 4, CBW_RIPS = 64 };
struct cbw_hit {
    uint64_t tsc, rip, at, field, value, frame, eop, bt[CBW_BT];
};
struct cbw_object {
    uint64_t tsc, outer, cb, ud, frame, eop;
    uint32_t birth; /* 1 at the init, 0 at a use */
};
struct cbw_thread {
    unsigned long id;
    uint64_t hit_count, object_count;
    struct cbw_hit hits[CBW_HITS];
    struct cbw_object objects[CBW_OBJECTS];
};
static unsigned char *cbw_site, *cbw_birth_site;
static struct cbw_thread cbw_pool[CBW_THREADS];
static volatile LONG cbw_pool_count;
static __thread struct cbw_thread *cbw_me;
static volatile LONG64 cbw_passes, cbw_births, cbw_hits_total, cbw_zero_hits;
static uint64_t cbw_rip_keys[CBW_RIPS], cbw_rip_values[CBW_RIPS];
static volatile LONG cbw_rip_counts[CBW_RIPS];
static struct cbw_thread *cbw_self(void) {
    if (!cbw_me) {
        const LONG k = InterlockedIncrement(&cbw_pool_count) - 1;
        if (k >= CBW_THREADS) return NULL;
        cbw_pool[k].id = GetCurrentThreadId();
        cbw_me = &cbw_pool[k];
    }
    return cbw_me;
}
static void cbw_stamp(uint64_t *frame, uint64_t *eop) {
    *frame = *eop = 0;
    if (gpu_enabled) gow3gpu_frame_counters(frame, eop);
}
static void cbw_dump_object(const char *what, uint64_t outer) {
    uint64_t q[16];
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)outer, q, sizeof(q), &got) || got != sizeof(q)) {
        fprintf(stderr, "CB watch: %s %#" PRIx64 ": unreadable\n", what, outer);
        return;
    }
    fprintf(stderr, "CB watch: %s %#" PRIx64 ":", what, outer);
    for (int i = 0; i < 16; ++i) fprintf(stderr, "%s%016" PRIx64, i == 8 ? "\n  +0x40:" : " ", q[i]);
    fprintf(stderr, "\n");
}
static void cbw_note_object(uint64_t outer, uint32_t birth) {
    struct cbw_thread *t = cbw_self();
    if (!t) return;
    if (!birth && t->object_count) {
        const struct cbw_object *last = &t->objects[(t->object_count - 1) % CBW_OBJECTS];
        if (last->outer == outer && !last->birth) return; /* the same object again */
    }
    struct cbw_object *o = &t->objects[t->object_count % CBW_OBJECTS];
    o->tsc = __rdtsc();
    o->outer = outer;
    o->cb = *(volatile uint64_t *)(uintptr_t)(outer + 0x18);
    o->ud = *(volatile uint64_t *)(uintptr_t)(outer + 0x20);
    o->birth = birth;
    cbw_stamp(&o->frame, &o->eop);
    ++t->object_count;
}
/* DR0/DR1 follow the current object, DR2/DR3 keep the one before it (set in the exception's
 * context: they apply when the thread resumes). */
static void cbw_arm_self(CONTEXT *c, uint64_t outer) {
    const DWORD64 on01 = 0x5, on23 = 0x50; /* local enables */
    if ((c->Dr7 & on01) == on01 && c->Dr0 == outer + 0x18) return;
    const DWORD64 previous = (c->Dr7 & on01) == on01 ? c->Dr0 : 0;
    if (previous) {
        c->Dr2 = previous;
        c->Dr3 = previous + 8;
        c->Dr7 = (c->Dr7 & ~((DWORD64)0xff << 24)) | on23 | ((DWORD64)0x99 << 24); /* write, 8 bytes */
    }
    c->Dr0 = outer + 0x18;
    c->Dr1 = outer + 0x20;
    c->Dr7 = (c->Dr7 & ~((DWORD64)0xff << 16)) | on01 | ((DWORD64)0x99 << 16);
    c->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
}
/* A debug register fired: the write already happened, rip is the next instruction. */
static void cbw_note_write(CONTEXT *c) {
    struct cbw_thread *t = cbw_self();
    const DWORD64 drs[4] = {c->Dr0, c->Dr1, c->Dr2, c->Dr3};
    for (int d = 0; d < 4; ++d) {
        if (!(c->Dr6 & (1u << d))) continue;
        InterlockedIncrement64(&cbw_hits_total);
        const uint64_t at = drs[d], value = *(volatile uint64_t *)(uintptr_t)at;
        const uint64_t rip = c->Rip - (uint64_t)(uintptr_t)image;
        int slot = -1;
        for (int i = 0; i < CBW_RIPS && slot < 0; ++i) {
            const LONG64 seen = InterlockedCompareExchange64((volatile LONG64 *)&cbw_rip_keys[i], (LONG64)rip, 0);
            if (!seen || (uint64_t)seen == rip) slot = i;
        }
        if (slot >= 0) { InterlockedIncrement(&cbw_rip_counts[slot]); cbw_rip_values[slot] = value; }
        if (!t) continue;
        struct cbw_hit *h = &t->hits[t->hit_count % CBW_HITS];
        h->tsc = __rdtsc();
        h->rip = rip;
        h->at = at;
        h->field = (d & 1) ? 0x20 : 0x18;
        h->value = value;
        cbw_stamp(&h->frame, &h->eop);
        memset(h->bt, 0, sizeof(h->bt));
        if (!(d & 1) && !value) {
            InterlockedIncrement64(&cbw_zero_hits);
            uint64_t rbp = c->Rbp;
            for (int depth = 0; depth < CBW_BT && rbp; ++depth) {
                uintptr_t frame[2];
                SIZE_T got = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)rbp, frame, sizeof(frame), &got) ||
                    got != sizeof(frame) || frame[0] <= rbp) break;
                h->bt[depth] = frame[1];
                rbp = frame[0];
            }
        }
        ++t->hit_count;
    }
    c->Dr6 = 0;
}
static DWORD WINAPI cbw_monitor(LPVOID unused) {
    (void)unused;
    for (;;) {
        Sleep(60000);
        fprintf(stderr, "CB watch: %lld uses, %lld inits, %lld watched writes (%lld callbacks zeroed), %ld threads\n",
                (long long)cbw_passes, (long long)cbw_births, (long long)cbw_hits_total, (long long)cbw_zero_hits,
                (long)cbw_pool_count);
    }
    return 0;
}
static void cbw_dump_thread(const struct cbw_thread *t, uint64_t now) {
    char where[MAX_PATH + 64];
    fprintf(stderr, "CB watch: thread %lu, %llu watched writes, %llu objects (newest last; age in tsc ticks before the fault)\n",
            t->id, (unsigned long long)t->hit_count, (unsigned long long)t->object_count);
    for (uint64_t n = t->object_count > CBW_OBJECTS ? t->object_count - CBW_OBJECTS : 0; n < t->object_count; ++n) {
        const struct cbw_object *o = &t->objects[n % CBW_OBJECTS];
        fprintf(stderr, "  %s %#" PRIx64 " cb %#" PRIx64 " ud %#" PRIx64 ", frame %llu, eop flips %llu, age %llu\n",
                o->birth ? "init" : "use ", o->outer, o->cb, o->ud, (unsigned long long)o->frame,
                (unsigned long long)o->eop, (unsigned long long)(now - o->tsc));
    }
    for (uint64_t n = t->hit_count > CBW_HITS ? t->hit_count - CBW_HITS : 0; n < t->hit_count; ++n) {
        const struct cbw_hit *h = &t->hits[n % CBW_HITS];
        fprintf(stderr, "  write %#" PRIx64 " to +0x%x of %#" PRIx64 ", before eboot+%#" PRIx64 ", frame %llu, eop flips %llu, age %llu%s\n",
                h->value, (unsigned)h->field, h->at - h->field, h->rip, (unsigned long long)h->frame,
                (unsigned long long)h->eop, (unsigned long long)(now - h->tsc),
                h->field == 0x18 && !h->value ? "  <-- CALLBACK ZEROED" : "");
        for (int depth = 0; depth < CBW_BT && h->bt[depth]; ++depth) {
            describe_address(where, sizeof(where), (uintptr_t)h->bt[depth]);
            fprintf(stderr, "    #%d %s\n", depth, where);
        }
    }
}
static void cbw_dump_all(void) {
    if (!cbw_site) return;
    const uint64_t now = __rdtsc();
    uint64_t frame, eop;
    cbw_stamp(&frame, &eop);
    fprintf(stderr, "CB watch: %lld uses, %lld inits, %lld watched writes (%lld callbacks zeroed); now frame %llu, eop flips %llu\n",
            (long long)cbw_passes, (long long)cbw_births, (long long)cbw_hits_total, (long long)cbw_zero_hits,
            (unsigned long long)frame, (unsigned long long)eop);
    for (int i = 0; i < CBW_RIPS && cbw_rip_keys[i]; ++i)
        fprintf(stderr, "  before eboot+%#" PRIx64 ": %ld writes, last value %#" PRIx64 "\n", cbw_rip_keys[i],
                (long)cbw_rip_counts[i], cbw_rip_values[i]);
    /* The faulting thread first, with its current and previous objects as they are now. */
    if (cbw_me) {
        fprintf(stderr, "CB watch: the faulting thread:\n");
        cbw_dump_thread(cbw_me, now);
        if (cbw_me->object_count) cbw_dump_object("its last object", cbw_me->objects[(cbw_me->object_count - 1) % CBW_OBJECTS].outer);
    }
    const LONG count = cbw_pool_count < CBW_THREADS ? cbw_pool_count : CBW_THREADS;
    for (LONG i = 0; i < count; ++i)
        if (&cbw_pool[i] != cbw_me) cbw_dump_thread(&cbw_pool[i], now);
}
static void cbw_install(uint64_t image_size) {
    static const unsigned char use[] = {0x48, 0x89, 0xfb, 0x48, 0x8d, 0xbb, 0xd0, 0x02, 0x00, 0x00, 0xe8};
    static const unsigned char birth[] = {0x48, 0x89, 0x53, 0x18, 0x48, 0x89, 0x5b, 0x20,
                                          0x48, 0xc7, 0x43, 0x30, 0x00, 0x00, 0x00, 0x00, 0xc7, 0x43, 0x3c};
    if (image_size < 0x6422c7 || memcmp(image + 0x501e9a, use, sizeof(use)) || memcmp(image + 0x6422ac, birth, sizeof(birth))) {
        printf("CB watch: the Uncharted 1.02 command buffer code at 0x501e9d/0x6422b4 is not in this executable, off\n");
        return;
    }
    cbw_site = image + 0x501e9d;
    cbw_birth_site = image + 0x6422b4;
    *cbw_birth_site = 0xcc;
    *cbw_site = 0xcc;
    HANDLE h = CreateThread(NULL, 0, cbw_monitor, NULL, 0, NULL);
    if (h) CloseHandle(h);
    printf("CB watch: each thread watches its command buffer objects (init 0x6422b4, use 0x501e9d), printed at a crash (GOW3_CB_WATCH)\n");
}
/* uc3 GOW3_UC1_BIND_RETRY (Uncharted 1.02): the two crashes of a friend's NVIDIA PC (2026-10-22,
 * execute at 0 in NdJobWorkerThread) are one loop of the command buffer writer (eboot+0x63e260):
 * each entry of the shader's last binding group names its handler by a type (u16 at +0) in a table
 * of member functions (eboot+0x94a350), and the entry it read had type 0, which has no handler
 * (the list says 5 entries; the 5th started with zeros). Instead of the crash the thread waits a
 * few ms for the entry to be written and runs it again; if it still has no handler it skips it
 * (one binding missing for one draw). The list is printed, at the fault and after the wait.
 * Third crash (2026-10-23, last_run 16): entry 4 held floats/half-floats, type 0x255c, far past the
 * table, so the "handler" was float data and the call faulted on a non-canonical address at the
 * call itself (0x64011d, read of ffffffffffffffff). That case (and a call into unmapped memory) is
 * handled the same way; a type counts as having a handler only below 0x100 with a pointer into the
 * executable (or a small odd one, a virtual function's offset). */
static unsigned char *ubr_loop;
static uint64_t ubr_image_size;
static LONG ubr_count;
static int ubr_valid(const uint64_t *table, uint16_t type) {
    if (type >= 0x100) return 0;
    const uint64_t p = table[type * 2];
    if (p & 1) return p < 0x10000;
    return p && p >= (uint64_t)(uintptr_t)image && p < (uint64_t)(uintptr_t)image + ubr_image_size;
}
static void ubr_install(uint64_t image_size) {
    static const unsigned char loop[] = {0x44, 0x89, 0xe0, 0x48, 0x8d, 0x0c, 0x40, 0x48, 0xc1, 0xe1, 0x04, 0x49, 0x8d, 0x44,
        0x0b, 0x28, 0x41, 0x0f, 0xb7, 0x54, 0x0b, 0x28, 0x48, 0xc1, 0xe2, 0x04, 0x48, 0x8b, 0x0c, 0x1a, 0x48, 0x8b, 0x7c,
        0x1a, 0x08, 0x4c, 0x01, 0xef, 0x4d, 0x89, 0xde, 0xf6, 0xc1, 0x01, 0x74, 0x08, 0x48, 0x8b, 0x17, 0x48, 0x8b, 0x4c,
        0x11, 0xff, 0x0f, 0xb6, 0x70, 0x02, 0x4c, 0x89, 0xfa, 0xff, 0xd1, 0x41, 0xff, 0xc4, 0x4d, 0x89, 0xf3};
    if (image_size < 0x6400e0 + sizeof(loop) || memcmp(image + 0x6400e0, loop, sizeof(loop))) {
        printf("UC1 binding retry: the Uncharted 1.02 binding loop at 0x6400e0 is not in this executable, off\n");
        return;
    }
    ubr_loop = image + 0x6400e0;
    ubr_image_size = image_size;
    printf("UC1 binding retry: a shader binding with no handler (0x64011d) waits for its entry, else is skipped (GOW3_UC1_BIND_RETRY)\n");
}
static void ubr_dump(const char *when, const unsigned char *list) {
    fprintf(stderr, "UC1 binding retry: the list at %p %s:", (const void *)list, when);
    for (int i = 0; i < 0x118; i += 8) {
        if (i % 0x30 == 0x28 || i == 0) fprintf(stderr, "\n  +%03x:", i);
        fprintf(stderr, " %016llx", (unsigned long long)*(volatile const uint64_t *)(list + i));
    }
    fprintf(stderr, "\n");
}
/* kind: 8 an execute fault (at 0 or at another address the call jumped to), else a read fault at
 * the call itself (a non-canonical target reads as ffffffffffffffff) */
static int ubr_handle(CONTEXT *c, uint64_t kind, uint64_t address) {
    if (!ubr_loop) return 0;
    int pushed;
    if (kind == 8 && *(const uint64_t *)(uintptr_t)c->Rsp == (uint64_t)(uintptr_t)(ubr_loop + 0x3f) && c->Rip == c->Rcx) pushed = 1;
    else if (kind != 8 && c->Rip == (DWORD64)(uintptr_t)(ubr_loop + 0x3d) && address == ~0ull) pushed = 0;
    else return 0;
    const unsigned char *list = (const unsigned char *)(uintptr_t)c->R14;
    const uint64_t *table = (const uint64_t *)(uintptr_t)c->Rbx;
    const uint32_t i = (uint32_t)c->R12;
    volatile const uint16_t *type = (volatile const uint16_t *)(list + 0x28 + (uint64_t)i * 0x30);
    const uint16_t first = *type;
    const LONG n = InterlockedIncrement(&ubr_count);
    const int log = n <= 8 || n % 100 == 0;
    if (log) {
        fprintf(stderr, "UC1 binding retry #%ld (thread %lu): entry %u of %u has type %#x, no handler (call to %#llx)\n", (long)n,
                GetCurrentThreadId(), i, *(const uint32_t *)(list + 0x24), first, (unsigned long long)c->Rcx);
        ubr_dump("at the fault", list);
    }
    int waited = 0;
    while (waited < 16 && !ubr_valid(table, *type)) { Sleep(1); ++waited; }
    const uint16_t now = *type;
    if (pushed) c->Rsp += 8; /* the call's return address */
    if (ubr_valid(table, now)) {
        c->R11 = c->R14; c->Rip = (DWORD64)(uintptr_t)ubr_loop; /* the loop reads the entry again */
    } else {
        c->Rip = (DWORD64)(uintptr_t)(ubr_loop + 0x3f); /* the next entry */
    }
    if (log) {
        fprintf(stderr, "UC1 binding retry #%ld: after %d ms the type is %#x: %s\n", (long)n, waited, now,
                ubr_valid(table, now) ? "run again" : "skipped");
        if (now != first) ubr_dump("after the wait", list);
        fflush(stderr);
    }
    return 1;
}
/* GOW3_MOUSE_AIM_WATCH (U1 mouse diagnostic, runtime_pad.c): who writes the aim camera's look vectors
 * while the zoom settles. The aim hook asks (runtime_aim_watch) for write watchpoints on its own thread
 * (debug registers 0 and 1, 8 bytes each); every write is counted by its instruction (image offset). */
#define AW_RIPS 32
static volatile uint64_t aw_addr[2];
static volatile LONG aw_on, aw_used;
static volatile LONG64 aw_keys[2][AW_RIPS];
static volatile LONG aw_counts[2][AW_RIPS];
static volatile LONG64 aw_last[2]; /* the last writer of each address (image offset), for GOW3_MOUSE_U1_GUN_LOG */
#define AW_CODE 0xe0a1a1a1u
void runtime_aim_watch(uint64_t at0, uint64_t at1, int on) {
    if (on) { aw_addr[0] = at0; aw_addr[1] = at1; aw_used = 1; }
    aw_on = on;
    RaiseException(AW_CODE, 0, 0, NULL);
}
void runtime_aim_watch_report(const char *what) {
    for (int f = 0; f < 2; ++f) {
        char line[1024];
        int n = snprintf(line, sizeof(line), "Mouse: U1 zoom writers of %s %s:", f ? "+0x1830" : "+0x1680", what);
        for (int i = 0; i < AW_RIPS && aw_keys[f][i]; ++i) {
            if (n < (int)sizeof(line) - 40) n += snprintf(line + n, sizeof(line) - n, " %#llx x%ld", (unsigned long long)aw_keys[f][i], (long)aw_counts[f][i]);
            aw_counts[f][i] = 0; aw_keys[f][i] = 0;
        }
        puts(line);
    }
}
uint64_t runtime_aim_watch_last(int f) {
    return (uint64_t)InterlockedExchange64(&aw_last[f & 1], 0);
}
static int aw_handle(const EXCEPTION_RECORD *record, CONTEXT *c) {
    if (record->ExceptionCode == AW_CODE) {
        if (aw_on) {
            c->Dr0 = aw_addr[0]; c->Dr1 = aw_addr[1];
            c->Dr7 = (c->Dr7 & ~((DWORD64)0xff << 16)) | 0x5 | ((DWORD64)0x99 << 16); /* write, 8 bytes */
        } else c->Dr7 &= ~(DWORD64)0xf;
        c->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
        return 1;
    }
    if (!aw_used || record->ExceptionCode != EXCEPTION_SINGLE_STEP || !(c->Dr6 & 3)) return 0;
    if (!aw_on) { c->Dr7 &= ~(DWORD64)0xf; c->ContextFlags |= CONTEXT_DEBUG_REGISTERS; c->Dr6 = 0; return 1; }
    const uint64_t rip = c->Rip - (uint64_t)(uintptr_t)image;
    for (int f = 0; f < 2; ++f) {
        if (!(c->Dr6 & (1u << f))) continue;
        for (int i = 0; i < AW_RIPS; ++i) {
            const LONG64 seen = InterlockedCompareExchange64(&aw_keys[f][i], (LONG64)rip, 0);
            if (!seen || (uint64_t)seen == rip) { InterlockedIncrement(&aw_counts[f][i]); break; }
        }
        aw_last[f] = (LONG64)rip;
    }
    c->Dr6 = 0;
    return 1;
}
static int fatal_reported;
static LONG fatal_exception(EXCEPTION_POINTERS *info) {
    if (__atomic_exchange_n(&fatal_reported,1,__ATOMIC_ACQ_REL)) { Sleep(INFINITE); }
    const EXCEPTION_RECORD *record=info->ExceptionRecord;
    const CONTEXT *c=info->ContextRecord;
    char where[MAX_PATH+64];
    describe_address(where,sizeof(where),(uintptr_t)c->Rip);
    /* uc3: the faulting thread's name (the port names its host threads: GPU command thread,
     * draw recording, shader capture, copy threads...). GetThreadDescription needs Windows 10
     * 1607; looked up at run time so older systems still load. */
    char thread_name[128]="";
    {
        typedef HRESULT (WINAPI *GetDescription)(HANDLE, PWSTR *);
        GetDescription get=(GetDescription)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"),"GetThreadDescription");
        PWSTR description=NULL;
        if (get && SUCCEEDED(get(GetCurrentThread(),&description)) && description) {
            if (description[0]) snprintf(thread_name,sizeof(thread_name)," \"%ls\"",description);
            LocalFree(description);
        }
    }
    if (record->ExceptionCode==EXCEPTION_ACCESS_VIOLATION && record->NumberParameters>=2)
        fprintf(stderr,"Fault: %s access to %p at %s (thread %lu%s)\n",
                record->ExceptionInformation[0]==1 ? "write" : record->ExceptionInformation[0]==8 ? "execute" : "read",
                (void *)record->ExceptionInformation[1],where,GetCurrentThreadId(),thread_name);
    else
        fprintf(stderr,"Fault: exception 0x%08lx at %s (thread %lu%s)\n",record->ExceptionCode,where,GetCurrentThreadId(),thread_name);
    fprintf(stderr,"  rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n  rsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx\n",
            c->Rax,c->Rbx,c->Rcx,c->Rdx,c->Rsi,c->Rdi,c->Rbp,c->Rsp);
    if (gpu_enabled) gow3gpu_dump_guest_writes(info);
    cbw_dump_all();
    /* rbp chain (guest and host code keep frame pointers). */
    uintptr_t rbp=(uintptr_t)c->Rbp;
    for (int depth=0; depth<24 && rbp; ++depth) {
        uintptr_t frame[2];
        SIZE_T got=0;
        if (!ReadProcessMemory(GetCurrentProcess(),(void *)rbp,frame,sizeof(frame),&got) || got!=sizeof(frame) || frame[0]<=rbp) break;
        describe_address(where,sizeof(where),frame[1]);
        fprintf(stderr,"  #%d %s\n",depth,where);
        rbp=frame[0];
    }
    fflush(NULL);
    /* uc3: GOW3_FAULT_HOLD=<seconds> keeps the process alive after a fatal fault, so a debugger
     * can attach and read every thread (lldb -p PID -b -o 'thread backtrace all'). */
    const char *hold=getenv("GOW3_FAULT_HOLD");
    if (hold && atoi(hold)>0) {
        fprintf(stderr,"Fault: holding the process %d s for a debugger (GOW3_FAULT_HOLD)\n",atoi(hold));
        fflush(NULL);
        Sleep((DWORD)atoi(hold)*1000);
    }
    TerminateProcess(GetCurrentProcess(),3);
    return EXCEPTION_CONTINUE_SEARCH;
}
static LONG CALLBACK vectored_fault(EXCEPTION_POINTERS *info) {
    const EXCEPTION_RECORD *record=info->ExceptionRecord;
    DWORD code=record->ExceptionCode;
    /* uc3 GOW3_CB_WATCH: its int3 and its debug register writes. */
    if (cbw_site && code==EXCEPTION_BREAKPOINT && (unsigned char *)(uintptr_t)info->ContextRecord->Rip==cbw_site) {
        CONTEXT *c=info->ContextRecord;
        c->Rdi=c->Rbx+0x2d0; c->Rip+=7;
        InterlockedIncrement64(&cbw_passes);
        cbw_note_object(c->Rbx,0);
        cbw_arm_self(c,c->Rbx);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (cbw_birth_site && code==EXCEPTION_BREAKPOINT && (unsigned char *)(uintptr_t)info->ContextRecord->Rip==cbw_birth_site) {
        CONTEXT *c=info->ContextRecord;
        *(volatile uint64_t *)(uintptr_t)(c->Rbx+0x30)=0; c->Rip+=8;
        InterlockedIncrement64(&cbw_births);
        cbw_note_object(c->Rbx,1);
        cbw_arm_self(c,c->Rbx);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (aw_handle(record,info->ContextRecord)) return EXCEPTION_CONTINUE_EXECUTION;
    if (cbw_site && code==EXCEPTION_SINGLE_STEP && (info->ContextRecord->Dr6 & 0xf)) {
        cbw_note_write(info->ContextRecord);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code==EXCEPTION_ACCESS_VIOLATION && record->NumberParameters>=2) {
        /* uc3: a speculative read gives up first. Resolving its fault through GPU page tracking
         * would wait for the GPU thread (a readback) while the reader holds locks that thread
         * needs: a draw-preparation worker keeps the pipeline cache shared-locked, and the GPU
         * thread waited for it exclusively (Uncharted 3, New Game). The GPU thread redoes that
         * work itself. */
        if (ubr_handle(info->ContextRecord,record->ExceptionInformation[0],record->ExceptionInformation[1])) return EXCEPTION_CONTINUE_EXECUTION;
        if (!runtime_fault_recover && gpu_enabled && gow3gpu_handle_fault(info,(void *)record->ExceptionInformation[1]))
            return EXCEPTION_CONTINUE_EXECUTION;
        if (runtime_fault_recover) {
            /* A speculative guest memory read (GPU draw preparation) faulted: the thread resumes
             * in gow3_longjmp, outside the exception dispatcher, back at its recovery point. */
            unsigned long long *buffer=*runtime_fault_recover;
            runtime_fault_recover=NULL;
            CONTEXT *c=info->ContextRecord;
            c->Rsp=((c->Rsp-128)&~(DWORD64)15)-40;
            c->Rcx=(DWORD64)(uintptr_t)buffer;
            c->Rdx=1;
            c->Rip=(DWORD64)(uintptr_t)gow3_longjmp;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (code==EXCEPTION_ACCESS_VIOLATION || code==EXCEPTION_ILLEGAL_INSTRUCTION || code==EXCEPTION_PRIV_INSTRUCTION ||
        code==EXCEPTION_STACK_OVERFLOW || code==EXCEPTION_INT_DIVIDE_BY_ZERO || code==EXCEPTION_DATATYPE_MISALIGNMENT) {
        /* Faults in guest code, the loader or the runtime end the process with a report; drivers
         * and system libraries may handle their own exceptions. */
        uintptr_t rip=(uintptr_t)info->ContextRecord->Rip;
        HMODULE self=GetModuleHandleA(NULL), module=NULL;
        int ours=(image && rip-(uintptr_t)image<0x20000000) ||
                 (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)rip,&module) && module==self) ||
                 !module;
        if (ours) return fatal_exception(info);
    }
    /* GOW3_STRICT_HANDLES=1: a closed or invalid handle used anywhere is reported (diagnostics). */
    if (code==0xC0000008 /* STATUS_INVALID_HANDLE */) return fatal_exception(info);
    return EXCEPTION_CONTINUE_SEARCH;
}
static LONG WINAPI unhandled_fault(EXCEPTION_POINTERS *info) { return fatal_exception(info); }
/* GOW3_EOP_TICK_WAIT=1 (from SuperMedo's uncharted_pc 0.3.2, GPL). Naughty Dog's FrameBegin
 * (ndlib/render/fg-draw-mgr.cpp) asserts that the frame it recycles has its GPU end-of-pipe
 * tick. It never waits: on PS4 the GPU always keeps up. Here the GPU side can fall behind
 * (shaders building on a cold cache), so the assertion's branch is found through its message,
 * in any version of the three games, and the failed check waits (pause, check again) instead. */
static void patch_eop_tick_wait(uint64_t image_size) {
    static const char message[]="GetRenderFrameParams(frameToFreeInstancesFor)->m_gfxEopTick";
    unsigned char *text=NULL;
    for (uint64_t i=0;i+sizeof(message)<=image_size;++i)
        if (image[i]=='G' && !memcmp(image+i,message,sizeof(message))) { text=image+i; break; }
    if (!text) { printf("Frame pacing: EOP tick message not found\n"); return; }
    int patched=0;
    for (uint64_t at=0;at+7<=image_size;++at) {
        /* lea rdi, [rip+rel32] -> the message */
        if (image[at]!=0x48 || image[at+1]!=0x8d || image[at+2]!=0x3d) continue;
        int32_t rel; memcpy(&rel,image+at+3,4);
        if (image+at+7+rel!=text) continue;
        /* Up to 64 bytes before: cmp qword [reg+disp32], 0 (48/49 83 /7, mod 10) ; jne past it.
         * UC1 indexes the frame array: the same cmp with a SIB byte (one byte longer), and its
         * assert handler is a function pointer (mov rax, [rip+x]) instead of a direct call. */
        for (uint64_t back=10;back<=64 && back<=at;++back) {
            unsigned char *c=image+at-back;
            if ((c[0]!=0x48 && c[0]!=0x49) || c[1]!=0x83 || (c[2]&0xf8)!=0xb8) continue;
            unsigned length=(c[2]&7)==4 ? 9 : 8;
            if (c[length-1]!=0x00 || c[length]!=0x75) continue;
            unsigned char *jne=c+length, *ok=jne+2+(int8_t)jne[1];
            if (ok<=image+at) continue;
            if (jne[2]!=0xe8 && !(jne[2]==0x48 && jne[3]==0x8b && jne[4]==0x05)) continue;
            /* after the jne: pause; jmp back to the cmp */
            jne[2]=0xf3; jne[3]=0x90; jne[4]=0xeb; jne[5]=(unsigned char)(int8_t)(c-(jne+6));
            printf("Frame pacing: the EOP tick check at %#" PRIx64 " waits for the GPU\n",
                   (uint64_t)(c-image));
            ++patched;
            break;
        }
    }
    if (!patched) printf("Frame pacing: EOP tick check not found\n");
}
/* GOW3_JOB_SPIN=N (ucndc, off): Naughty Dog's job workers (NdJobWorkerThread 0-5, the same
 * loop in UC1, UC2 and UC3) look for work in their queues and, when there is none and sleeping
 * is allowed (byte [manager+0x59]), spin 1000 times with PAUSE before they block on a condition
 * variable (10 ms timeout). The game wakes them with a broadcast tens of thousands of times a
 * second, so on a PC, where PAUSE takes far longer than on the PS4's Jaguar, the six workers
 * stay near a full core each. This sets the 1000 (cmp r8, 1000 ; jl spin) to N; 0 blocks at
 * once. Found by its bytes: cmp byte [rdi+0x59],0 ; je ; cmp r8,0x3e8 ; jl.
 * GOW3_JOB_SLEEP=1 (ucndc, off): the game clears that byte during gameplay (function at UC3
 * 0xd96440, which also drops the workers to the lowest priority, 767) and the workers then spin
 * with PAUSE forever, never blocking; it is set in cutscenes and loading, where the game's CPU
 * use falls to ~5%. This removes the je, so the workers always spin N times and then block. */
static void patch_job_spin(uint64_t image_size, const char *value, int sleep) {
    char *end = NULL;
    long n = value ? strtol(value, &end, 0) : 1000;
    if (value && (!value[0] || *end || n < 0 || n > 0x7fffffff)) { printf("Job spin: bad GOW3_JOB_SPIN=%s\n", value); return; }
    static const unsigned char head[] = {0x80, 0x7f, 0x59, 0x00, 0x74};
    static const unsigned char cmp[] = {0x49, 0x81, 0xf8, 0xe8, 0x03, 0x00, 0x00, 0x7c};
    uint64_t found = 0, count = 0;
    for (uint64_t at = 0; at + sizeof(head) + 1 + sizeof(cmp) <= image_size; ++at) {
        if (image[at] != 0x80 || memcmp(image + at, head, sizeof(head)) ||
            memcmp(image + at + sizeof(head) + 1, cmp, sizeof(cmp))) continue;
        found = at + sizeof(head) + 1;
        ++count;
    }
    if (count != 1) { printf("Job spin: worker wait loop %s\n", count ? "found more than once, left alone" : "not found"); return; }
    uint32_t spins = (uint32_t)n;
    memcpy(image + found + 3, &spins, 4);
    if (sleep) { image[found - 2] = 0x66; image[found - 1] = 0x90; } /* je -> 2-byte nop */
    printf("Job spin: job workers spin %ld times before they sleep (was 1000)%s, at %#" PRIx64 "\n", n,
           sleep ? ", and may sleep in gameplay too" : "", found);
}
/* GOW3_UC3_RES=WxH (ucndc, off; Opus's resolution study, uc3/resolucao/estudo-resolucao.md):
 * Uncharted 3's video settings (a structure at 0x18c6b34, written by the function at 0xe39690)
 * hold the output size (+0x08), the render sizes (+0x18, +0x24, +0x38, 1920x1080) and the largest
 * buffer size (+0x30, 2560x1216, also written at 0x565002 and 0x565043). Each is a
 * mov dword [rax/rcx+off], imm32 whose immediate is replaced here, only when every one of them
 * holds the expected bytes. GOW3_UC3_RES_ALL=1 also changes the output size.
 * GOW3_UC3_RES_LOG=1 prints the structure's first 0x88 bytes once mapped and on each change. */
typedef struct { uint64_t at; unsigned char reg, off; uint32_t value; int kind; } Uc3ResSite;
enum { UC3_RES_OUT, UC3_RES_W, UC3_RES_H, UC3_RES_MAXW, UC3_RES_MAXH, UC3_RES_OUTH };
static const Uc3ResSite uc3_res_sites[] = {
    {0xe396b3, 0x40, 0x08, 0x780, UC3_RES_OUT},  {0xe396ba, 0x40, 0x0c, 0x438, UC3_RES_OUTH},
    {0xe396c8, 0x40, 0x18, 0x780, UC3_RES_W},    {0xe396cf, 0x40, 0x1c, 0x438, UC3_RES_H},
    {0xe396dd, 0x40, 0x24, 0x780, UC3_RES_W},    {0xe396e4, 0x40, 0x28, 0x438, UC3_RES_H},
    {0xe39715, 0x40, 0x30, 0xa00, UC3_RES_MAXW}, {0xe3971c, 0x40, 0x34, 0x4c0, UC3_RES_MAXH},
    {0xe39723, 0x40, 0x38, 0x780, UC3_RES_W},    {0xe3972a, 0x40, 0x3c, 0x438, UC3_RES_H},
    {0x565002, 0x40, 0x30, 0xa00, UC3_RES_MAXW}, {0x565009, 0x40, 0x34, 0x4c0, UC3_RES_MAXH},
    {0x56504a, 0x41, 0x30, 0xa00, UC3_RES_MAXW}, {0x565051, 0x41, 0x34, 0x4c0, UC3_RES_MAXH},
};
/* Uncharted 2 and 1 (Opus, uc3/resolucao/estudo-uc1-uc2.md): the same init (update: UC2 0xbd2300,
 * UC1 0x4d0710), found by its first two stores; the sites sit at the same distances as UC3's from 0xe396b3,
 * and the options code has no second write of the largest buffer size. The structure's address comes
 * from the lea rax,[rip+disp] that starts the init, 0x23 bytes before. */
static const Uc3ResSite uc2_res_sites[] = {
    {0x00, 0x40, 0x08, 0x780, UC3_RES_OUT},  {0x07, 0x40, 0x0c, 0x438, UC3_RES_OUTH},
    {0x15, 0x40, 0x18, 0x780, UC3_RES_W},    {0x1c, 0x40, 0x1c, 0x438, UC3_RES_H},
    {0x2a, 0x40, 0x24, 0x780, UC3_RES_W},    {0x31, 0x40, 0x28, 0x438, UC3_RES_H},
    {0x62, 0x40, 0x30, 0xa00, UC3_RES_MAXW}, {0x69, 0x40, 0x34, 0x4c0, UC3_RES_MAXH},
    {0x70, 0x40, 0x38, 0x780, UC3_RES_W},    {0x77, 0x40, 0x3c, 0x438, UC3_RES_H},
};
#define UC3_RES_STRUCT 0x18c6b34u
#define UC_RES_MAX_SITES 16
static uint64_t uc_find_unique(uint64_t image_size, const void *bytes, size_t len) {
    const unsigned char *b = bytes;
    uint64_t found = 0, count = 0;
    for (uint64_t at = 0; at + len <= image_size; ++at)
        if (image[at] == b[0] && !memcmp(image + at, b, len)) { found = at; ++count; }
    return count == 1 ? found : 0;
}
static int uc_res_site_ok(uint64_t image_size, const Uc3ResSite *s) {
    unsigned char expect[7] = {0xc7, s->reg, s->off};
    memcpy(expect + 3, &s->value, 4);
    return s->at + 7 <= image_size && !memcmp(image + s->at, expect, 7);
}
/* The video settings' sites and structure: UC3's at fixed addresses, else UC2's by the init's bytes.
 * Returns the number of sites, 0 when neither matches. */
static size_t uc_res_find(uint64_t image_size, Uc3ResSite *sites, uint64_t *structure, const char **game) {
    size_t n = sizeof(uc3_res_sites) / sizeof(uc3_res_sites[0]);
    int ok = 1;
    for (size_t i = 0; i < n; ++i) ok &= uc_res_site_ok(image_size, &uc3_res_sites[i]);
    if (ok) {
        memcpy(sites, uc3_res_sites, sizeof(uc3_res_sites));
        *structure = UC3_RES_STRUCT;
        *game = "UC3";
        return n;
    }
    static const unsigned char init[] = {0xc7, 0x40, 0x08, 0x80, 0x07, 0, 0, 0xc7, 0x40, 0x0c, 0x38, 0x04, 0, 0};
    const uint64_t base = uc_find_unique(image_size, init, sizeof(init));
    if (base < 0x23 || memcmp(image + base - 0x23, "\x48\x8d\x05", 3)) return 0;
    n = sizeof(uc2_res_sites) / sizeof(uc2_res_sites[0]);
    for (size_t i = 0; i < n; ++i) {
        sites[i] = uc2_res_sites[i];
        sites[i].at += base;
        if (!uc_res_site_ok(image_size, &sites[i])) return 0;
    }
    int32_t disp;
    memcpy(&disp, image + base - 0x23 + 3, 4);
    *structure = base - 0x23 + 7 + (int64_t)disp;
    *game = "UC1/UC2";
    /* UC3 1.00 rewrites the largest buffer size in its options code once (0x15e18c, mov [rbx+0x30/0x34]). */
    static const unsigned char max[] = {0xc7, 0x43, 0x30, 0x00, 0x0a, 0, 0, 0xc7, 0x43, 0x34, 0xc0, 0x04, 0, 0};
    const uint64_t at = uc_find_unique(image_size, max, sizeof(max));
    if (at) {
        sites[n++] = (Uc3ResSite){at, 0x43, 0x30, 0xa00, UC3_RES_MAXW};
        sites[n++] = (Uc3ResSite){at + 7, 0x43, 0x34, 0x4c0, UC3_RES_MAXH};
        *game = "UC3";
    }
    return n;
}
static void patch_uc3_resolution(const Uc3ResSite *sites, size_t n, const char *game, const char *value, int all) {
    unsigned w = 0, h = 0;
    char extra = 0;
    if (sscanf(value, "%ux%u%c", &w, &h, &extra) != 2 || w < 640 || h < 360 || w > 7680 || h > 4320) {
        printf("UC3 resolution: bad GOW3_UC3_RES=%s (WxH)\n", value);
        return;
    }
    if (!n) {
        printf("UC3 resolution: the video settings were not found as expected, not patched (another game or version)\n");
        return;
    }
    const uint32_t maxw = w > 0xa00 ? w : 0xa00, maxh = h > 0x4c0 ? h : 0x4c0;
    for (size_t i = 0; i < n; ++i) {
        const Uc3ResSite *s = &sites[i];
        uint32_t v = s->value;
        switch (s->kind) {
        case UC3_RES_W: v = w; break;
        case UC3_RES_H: v = h; break;
        case UC3_RES_MAXW: v = maxw; break;
        case UC3_RES_MAXH: v = maxh; break;
        case UC3_RES_OUT: if (all) v = w; break;
        case UC3_RES_OUTH: if (all) v = h; break;
        }
        memcpy(image + s->at + 3, &v, 4);
    }
    printf("%s resolution: render %ux%u, largest buffer %ux%u, output %s (%zu sites from %#" PRIx64 ")\n", game, w, h,
           maxw, maxh, all ? value : "1920x1080 (unchanged)", n, sites[0].at);
}
/* GOW3_UC3_RTHEAP_MB=N and GOW3_UC3_RT_MB=M (Opus, 1440p/4K): the memory map table in .data
 * (0x16f8d90..0x16fa050, 32-byte entries {u32 id, u32 group, u64 size, u64 align, u64 0}) sizes the
 * game's heaps for 1080p. N MB are added to the Render Target Heap (group 6, id 0x85, 0x2310bc00) and
 * M MB to the Render Target heap (id 0x84, 0x8bc4000), and N+M to their region (group 2, id 3,
 * 0xbec00000 = 3052 MB, the sum of group 6), so the GPU memory the game allocates grows by N+M; the
 * rest (id 0xe, used later by the game) is left alone. All sizes must read as expected. The group 2
 * context total (0x120000000 = 4608 MB, the movabs imm64 at 0x316e95 passed to the context setup at
 * 0xd9dec0) grows by N+M as well, or ValidateContext asserts "Size Does Not Add Up". The direct
 * memory pool (5056 MiB) had about 468 MiB free at 1080p; above that GOW3_DMEM_MB must grow too.
 * The game places its memory areas one after another, each at the next 4 GiB boundary (the next
 * address at 0x16e7920, aligned up after each AllocateMemory at 0xdaaa80), so a GPU region above
 * 4 GiB moves the flexible memory after it; the game asserts that each lands where it asked.
 * Above 4 GiB the heap init at 0xd9b1b0 truncates the size to 32 bits (mov eax, eax at 0xd9b269,
 * found by Opus) before it sets the arena's end and size fields; that move becomes a 2-byte nop
 * (the bytes must read as expected; the other heap inits are left alone). */
#define UC3_RT_SIZE 0x16f9bf8u
#define UC3_RTHEAP_SIZE 0x16f9c18u
#define UC3_GPU_REGION_SIZE 0x16f8dd8u
#define UC3_DIRECT_TOTAL_IMM 0x316e95u
#define UC3_HEAP_SIZE_TRUNC 0xd9b269u
static int uc3_mb(const char *name, unsigned long *mb) {
    const char *value = getenv(name);
    *mb = 0;
    if (!value || !value[0]) return 1;
    char *end = NULL;
    *mb = strtoul(value, &end, 10);
    if (!end || *end || *mb > 4096) {
        printf("UC3 render target heap: bad %s=%s (0-4096)\n", name, value);
        return 0;
    }
    return 1;
}
/* Uncharted 2 and 1 (Opus, uc3/resolucao/estudo-uc1-uc2.md; the update's executables): the same table
 * with their own sizes (Render Target Heap 0x1f10bc00, region 0xbac00000), each entry found by its id,
 * group and size, the same movabs for the total, and the same GPU Mem heap init as UC3's 0xd9b1b0
 * (UC2 0xb2f9b0, UC1 0x4503a0), whose mov eax, eax is found by the bytes around it: jmp back to the
 * spinlock, lfence, mov [r13+0x60], r14, mov rax, [rbp-0x40]. */
static const unsigned char uc_heap_trunc[] = {0xeb, 0xd2, 0x0f, 0xae, 0xe8, 0x4d, 0x89, 0x75,
                                              0x60, 0x48, 0x8b, 0x45, 0xc0, 0x89, 0xc0};
/* The 1.00 executables (Opus, "Desenho para a 1.00") truncate elsewhere: the inner heap init (UC3
 * 0x4957b0, UC2 0x4e1ef0, UC1 0x35c8d0) keeps the size in a 32-bit local. Opus's 33-byte rewrite at
 * its +0x6e reads the 64-bit size the GPU Mem wrapper stored at [obj+0xc8] (r13+0x78) when its low
 * half matches the size passed, and the 32-bit one otherwise (another class calls the same init);
 * the lfence after the spinlock's lock cmpxchg goes to make room. Used only when the update's
 * mov eax, eax is not there (the update has these bytes too, in an init that is not GPU Mem's). */
static const unsigned char uc_heap_trunc_100[] = {
    0x0f, 0xae, 0xe8, 0x4d, 0x89, 0x75, 0x10, 0x8b, 0x45, 0xc4, 0x49, 0x8b, 0x4d, 0x18, 0x48, 0x8d, 0x14,
    0x85, 0x00, 0x00, 0x00, 0x00, 0x48, 0x83, 0xe1, 0x03, 0x48, 0x09, 0xd1, 0x49, 0x89, 0x4d, 0x18};
static const unsigned char uc_heap_wide_100[] = {
    0x4d, 0x89, 0x75, 0x10, 0x49, 0x8b, 0x45, 0x78, 0x3b, 0x45, 0xc4, 0x74, 0x03, 0x8b, 0x45, 0xc4, 0x49,
    0x8b, 0x4d, 0x18, 0x83, 0xe1, 0x03, 0x48, 0x8d, 0x0c, 0x81, 0x49, 0x89, 0x4d, 0x18, 0x66, 0x90};
static uint64_t uc_find_entry(uint64_t image_size, uint32_t id, uint32_t group, uint64_t size) {
    unsigned char entry[16];
    memcpy(entry, &id, 4);
    memcpy(entry + 4, &group, 4);
    memcpy(entry + 8, &size, 8);
    const uint64_t at = uc_find_unique(image_size, entry, sizeof(entry));
    return at ? at + 8 : 0;
}
static void patch_uc3_rt_heap(uint64_t image_size) {
    unsigned long mb_heap = 0, mb_rt = 0;
    if (!uc3_mb("GOW3_UC3_RTHEAP_MB", &mb_heap) || !uc3_mb("GOW3_UC3_RT_MB", &mb_rt) || !(mb_heap + mb_rt))
        return;
    uint64_t at_heap = UC3_RTHEAP_SIZE, at_rt = UC3_RT_SIZE, at_region = UC3_GPU_REGION_SIZE,
             at_total = UC3_DIRECT_TOTAL_IMM;
    uint64_t heap = 0, rt = 0, region = 0, total = 0;
    const char *game = "UC3";
    int fixed = 1;
    if (UC3_RTHEAP_SIZE + 8 <= image_size) {
        memcpy(&heap, image + at_heap, 8);
        memcpy(&rt, image + at_rt, 8);
        memcpy(&region, image + at_region, 8);
        memcpy(&total, image + at_total, 8);
    }
    if (heap != 0x2310bc00u || rt != 0x8bc4000u || region != 0xbec00000u || total != 0x120000000ull ||
        memcmp(image + at_total - 2, "\x48\xba", 2)) {
        at_heap = uc_find_entry(image_size, 0x85, 6, 0x2310bc00u);
        at_region = uc_find_entry(image_size, 3, 2, 0xbec00000u);
        game = "UC3";
        if (!at_heap || !at_region) {
            at_heap = uc_find_entry(image_size, 0x85, 6, 0x1f10bc00u);
            at_region = uc_find_entry(image_size, 3, 2, 0xbac00000u);
            game = "UC1/UC2";
        }
        at_rt = uc_find_entry(image_size, 0x84, 6, 0x8bc4000u);
        at_total = uc_find_unique(image_size, "\x48\xba\x00\x00\x00\x20\x01\x00\x00\x00", 10);
        if (!at_heap || !at_rt || !at_region || !at_total) {
            printf("UC3 render target heap: unexpected sizes %#" PRIx64 "/%#" PRIx64 "/%#" PRIx64 "/%#" PRIx64
                   " and no table found by its bytes, not patched\n", heap, rt, region, total);
            return;
        }
        at_total += 2;
        memcpy(&heap, image + at_heap, 8);
        memcpy(&rt, image + at_rt, 8);
        memcpy(&region, image + at_region, 8);
        memcpy(&total, image + at_total, 8);
        fixed = 0;
    }
    const uint64_t add_heap = (uint64_t)mb_heap << 20, add_rt = (uint64_t)mb_rt << 20;
    const uint64_t new_heap = heap + add_heap, new_rt = rt + add_rt;
    const uint64_t new_region = region + add_heap + add_rt, new_total = total + add_heap + add_rt;
    memcpy(image + at_heap, &new_heap, 8);
    memcpy(image + at_rt, &new_rt, 8);
    memcpy(image + at_region, &new_region, 8);
    memcpy(image + at_total, &new_total, 8);
    const char *trunc = "";
    if (new_region > (UINT64_C(1) << 32) && fixed) {
        if (!memcmp(image + UC3_HEAP_SIZE_TRUNC - 4, "\x48\x8b\x45\xc0\x89\xc0", 6)) {
            memcpy(image + UC3_HEAP_SIZE_TRUNC, "\x66\x90", 2);
            trunc = " (above 4 GiB: 64-bit heap size at 0xd9b269)";
        } else {
            trunc = " (above 4 GiB, but the heap init at 0xd9b269 is not as expected: sizes wrap)";
        }
    } else if (new_region > (UINT64_C(1) << 32)) {
        const uint64_t at = uc_find_unique(image_size, uc_heap_trunc, sizeof(uc_heap_trunc));
        const uint64_t at_100 = at ? 0 : uc_find_unique(image_size, uc_heap_trunc_100, sizeof(uc_heap_trunc_100));
        if (at) {
            memcpy(image + at + sizeof(uc_heap_trunc) - 2, "\x66\x90", 2);
            trunc = " (above 4 GiB: 64-bit heap size in the GPU Mem heap init)";
        } else if (at_100) {
            memcpy(image + at_100, uc_heap_wide_100, sizeof(uc_heap_wide_100));
            trunc = " (above 4 GiB: 64-bit heap size in the 1.00 heap init)";
        } else {
            trunc = " (above 4 GiB, but the GPU Mem heap init is not as expected: sizes wrap)";
        }
    }
    printf("%s render target heap: %" PRIu64 " -> %" PRIu64 " bytes (+%lu MB), render targets %" PRIu64 " -> %" PRIu64
           " (+%lu MB), GPU region %" PRIu64 " -> %" PRIu64 ", direct total %" PRIu64 " -> %" PRIu64 "%s\n",
           game, heap, new_heap, mb_heap, rt, new_rt, mb_rt, region, new_region, total, new_total, trunc);
}
static uint64_t uc_res_struct = UC3_RES_STRUCT;
static void *uc3_res_log(void *unused) {
    (void)unused;
    uint32_t last[0x22] = {0};
    int printed = 0;
    for (;;) {
#ifdef _WIN32
        Sleep(500);
#else
        usleep(500000);
#endif
        uint32_t now[0x22];
        memcpy(now, image + uc_res_struct, sizeof(now));
        if (printed && !memcmp(now, last, sizeof(now))) continue;
        int nonzero = 0;
        for (size_t i = 0; i < 0x22; ++i) nonzero |= now[i] != 0;
        if (!nonzero) continue;
        char line[1024];
        int k = snprintf(line, sizeof(line), "UC3 resolution struct %s:", printed ? "changed" : "");
        for (size_t i = 0; i < 0x22 && k < (int)sizeof(line) - 16; ++i)
            k += snprintf(line + k, sizeof(line) - k, "%s+%02zx=%u", i % 8 ? " " : "\n  ", i * 4, now[i]);
        printf("%s\n", line);
        fflush(stdout);
        memcpy(last, now, sizeof(now));
        printed = 1;
    }
    return NULL;
}
/* Mouse camera callbacks (runtime_guest_callbacks in runtime_pad.c; from SuperMedo's uncharted_pc,
 * GPL): a guest function found by its code calls a host function first, with rdi, rsi, the image
 * base and r14 (Microsoft x64 convention on every host). The thunk keeps the guest's registers and
 * flags, so it also works inside a function. ucndc: the code is searched for in the whole
 * executable (one pattern per game version, 1.00 and 1.02) instead of at fixed offsets, and a
 * pattern found other than exactly once is left alone. */
typedef struct { unsigned char bytes[64], mask[64]; size_t size; } CodePattern;
static int parse_code_pattern(const char *text, CodePattern *p) {
    p->size=0;
    while (*text) {
        if (*text==' ') { ++text; continue; }
        if (p->size==sizeof(p->bytes) || !text[1]) return 0;
        if (text[0]=='?' && text[1]=='?') { p->bytes[p->size]=0; p->mask[p->size]=0; }
        else {
            char hex[3]={text[0],text[1],0}, *end=NULL;
            p->bytes[p->size]=(unsigned char)strtoul(hex,&end,16);
            if (*end) return 0;
            p->mask[p->size]=0xff;
        }
        ++p->size;
        text+=2;
    }
    return p->size>0;
}
static int code_matches(const unsigned char *at, const CodePattern *p) {
    for (size_t i=0;i<p->size;++i) if ((at[i]&p->mask[i])!=p->bytes[i]) return 0;
    return 1;
}
static int in_code(const Segment *segments, uint64_t ns, uint64_t address, uint64_t bytes) {
    for (uint64_t s=0;s<ns;++s)
        if ((segments[s].flags&1) && address>=segments[s].address && address+bytes<=segments[s].address+segments[s].size) return 1;
    return 0;
}
static unsigned char *near_image_memory(uint64_t image_size, size_t size) {
    unsigned char *memory=NULL;
#ifdef _WIN32
    /* rel32 jumps: within 2 GiB of the image. */
    uint64_t base=((uint64_t)(uintptr_t)image+round_page(image_size)+(32u<<20)+0xffff)&~(uint64_t)0xffff;
    for (uint64_t step=0;!memory && step<(1900ull<<20);step+=1u<<16) {
        memory=VirtualAlloc((void *)(uintptr_t)(base+step),size,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
        if (!memory && (uint64_t)(uintptr_t)image>step+(2u<<20))
            memory=VirtualAlloc((void *)(uintptr_t)(((uint64_t)(uintptr_t)image-step-(2u<<20))&~(uint64_t)0xffff),
                                size,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
    }
#else
    (void)image_size; (void)size;
#endif
    return memory;
}
volatile uint64_t runtime_guest_rbp, runtime_guest_ret, runtime_image_base, runtime_guest_r14;
/* GOW3_MOUSE_U1_CALL98=1 (UC1 1.02 diagnostic, off by default): every "call *0x98(%reg)" in the code
 * (the slot the camera manager updates the cameras through) gets a thunk that reports the site, the
 * function about to be called and rdi/rsi to runtime_call98_seen (runtime_pad.c), to find the camera
 * update of the mounted gun, which none of the census classes runs (Fable, 2026-10-23). */
static void install_call98_log(const Segment *segments, uint64_t ns, uint64_t image_size) {
    const char *v=getenv("GOW3_MOUSE_U1_CALL98");
    if (!(v && v[0]=='1')) return;
    static const unsigned char check[]={0xff,0x90,0x98,0x00,0x00,0x00};
    if (image_size<0x7ab30 || memcmp(image+0x7ab24,check,sizeof(check))) { printf("Call98: not UC1 1.02, off\n"); return; }
    unsigned char *thunks=near_image_memory(image_size,1u<<17);
    if (!thunks) { printf("Call98: no thunk memory\n"); return; }
    size_t used=0;
    int installed=0, skipped=0;
    static uint64_t cells[256];
    /* the 68 sites in UC1 1.02, each checked as a whole instruction with objdump (no stray byte matches) */
    static const uint64_t sites[]={0xde22,0x1177b,0x1479d,0x2fe51,0x2fec9,0x3040d,0x3047a,0x304bc,0x61c16,0x7ab24,0xe8630,0xe92de,0xe9361,0x147fbb,0x148083,0x148144,0x254c57,0x255057,0x26408e,0x2710b2,0x3193ef,0x3264cd,0x44f7a8,0x450a0a,0x451f48,0x4521b6,0x457aaa,0x4657c6,0x4658b6,0x465920,0x465938,0x472aa1,0x49cf4b,0x4ebee0,0x4ef03c,0x519ad6,0x51a5a4,0x545d8b,0x545f79,0x64cfff,0x64d7e9,0x64e429,0x6a0acb,0x6b83c4,0x6bea17,0x6bf110,0x6c22f0,0x6d340b,0x6dd3a6,0x6e3936,0x6e4063,0x6f276d,0x6f9015,0x6fd840,0x7010f1,0x702ad9,0x7039c9,0x70467a,0x70d641,0x71e33b,0x71e778,0x71eba6,0x72751c,0x729441,0x72aa81,0x72bbc4,0x74faf9,0x74fd43};
    runtime_image_base=(uint64_t)(uintptr_t)image;
    for (size_t si=0;si<sizeof(sites)/sizeof(sites[0]);++si) {
        {
            const uint64_t at=sites[si];
            if (!in_code(segments,ns,at,7)) { ++skipped; continue; }
            const unsigned char *f=image+at;
            const int rex=f[0]==0x41;
            const unsigned char *c=f+rex;
            if (c[0]!=0xff || (c[1]&0xf8)!=0x90 || c[2]!=0x98 || c[3]||c[4]||c[5]) { ++skipped; continue; }
            const int reg=(c[1]&7)+(rex ? 8 : 0);
            const size_t moved=6+(size_t)rex;
            if ((reg&7)==4 || installed>=256) { ++skipped; continue; }
            if (used+400>(1u<<17)) { ++skipped; continue; }
            unsigned char *t=thunks+used;
            size_t k=0;
            const uintptr_t cell=(uintptr_t)&cells[installed];
            t[k++]=0x50;                                                   /* push rax */
            t[k++]=(unsigned char)(0x48|(reg>=8 ? 1 : 0)); t[k++]=0x8b;     /* mov rax, [reg+0x98] */
            t[k++]=(unsigned char)(0x80|(reg&7));
            { const uint32_t disp=0x98; memcpy(t+k,&disp,4); k+=4; }
            t[k++]=0x48; t[k++]=0xa3; memcpy(t+k,&cell,8); k+=8;          /* movabs [cell], rax */
            t[k++]=0x58;                                                   /* pop rax */
            static const unsigned char save[]={0x9c,0x50,0x51,0x52,0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53,0x53,
                                               0x48,0x89,0xe3,0x48,0x83,0xe4,0xf0,0x48,0x81,0xec,0xa0,0x00,0x00,0x00};
            memcpy(t+k,save,sizeof(save)); k+=sizeof(save);
            for (int x=0;x<8;++x) {
                const uint32_t disp=0x20u+16u*(uint32_t)x;
                t[k++]=0xf3; t[k++]=0x0f; t[k++]=0x7f; t[k++]=(unsigned char)(0x84|x<<3); t[k++]=0x24;
                memcpy(t+k,&disp,4); k+=4;
            }
            t[k++]=0x48; t[k++]=0x89; t[k++]=0xf9;                         /* mov rcx, rdi */
            t[k++]=0x48; t[k++]=0x89; t[k++]=0xf2;                         /* mov rdx, rsi */
            { const uint64_t site=at; t[k++]=0x49; t[k++]=0xb8; memcpy(t+k,&site,8); k+=8; } /* movabs r8, site */
            t[k++]=0x48; t[k++]=0xb8; memcpy(t+k,&cell,8); k+=8;           /* movabs rax, cell */
            t[k++]=0x4c; t[k++]=0x8b; t[k++]=0x08;                         /* mov r9, [rax] */
            { const uintptr_t host=(uintptr_t)runtime_call98_seen; t[k++]=0x48; t[k++]=0xb8; memcpy(t+k,&host,8); k+=8; }
            t[k++]=0xff; t[k++]=0xd0;                                      /* call rax */
            for (int x=0;x<8;++x) {
                const uint32_t disp=0x20u+16u*(uint32_t)x;
                t[k++]=0xf3; t[k++]=0x0f; t[k++]=0x6f; t[k++]=(unsigned char)(0x84|x<<3); t[k++]=0x24;
                memcpy(t+k,&disp,4); k+=4;
            }
            static const unsigned char restore[]={0x48,0x89,0xdc,0x5b,0x41,0x5b,0x41,0x5a,0x41,0x59,0x41,0x58,
                                                  0x5a,0x59,0x58,0x9d};
            memcpy(t+k,restore,sizeof(restore)); k+=sizeof(restore);
            memcpy(t+k,f,moved); k+=moved;                                 /* the call itself */
            int32_t back=(int32_t)((int64_t)(uintptr_t)(f+moved)-(int64_t)(uintptr_t)(t+k+5));
            t[k++]=0xe9; memcpy(t+k,&back,4); k+=4;
            unsigned char *w=image+at;
            int32_t to=(int32_t)((int64_t)(uintptr_t)t-(int64_t)(uintptr_t)(w+5));
            w[0]=0xe9; memcpy(w+1,&to,4); memset(w+5,0x90,moved-5);
            used+=(k+15)&~(size_t)15;
            ++installed;
        }
    }
    printf("Call98: %d call sites logged, %d skipped\n",installed,skipped);
}
static void install_game_callbacks(const Segment *segments, uint64_t ns, uint64_t image_size) {
    size_t count=0;
    const RuntimeGuestCallback *callbacks=runtime_guest_callbacks(&count);
    unsigned char *thunks=NULL;
    size_t used=0;
    int installed=0;
    for (size_t i=0;i<count;++i) {
        const RuntimeGuestCallback *c=&callbacks[i];
        CodePattern p, need;
        if (!parse_code_pattern(c->pattern,&p) || (c->require && !parse_code_pattern(c->require,&need)) ||
            c->moved<5 || c->moved>p.size) {
            printf("Callbacks: %s: bad pattern\n",c->name);
            continue;
        }
        uint64_t found=0, matches=0;
        for (uint64_t s=0;s<ns;++s) {
            if (!(segments[s].flags&1)) continue;
            const uint64_t start=segments[s].address, end=start+segments[s].size;
            for (uint64_t at=start;at+p.size<=end;++at) {
                if (image[at]!=p.bytes[0] || !code_matches(image+at,&p)) continue;
                if (c->require) {
                    const uint64_t r=at+(uint64_t)c->require_at;
                    if (!in_code(segments,ns,r,need.size) || !code_matches(image+r,&need)) continue;
                }
                found=at;
                ++matches;
            }
        }
        if (matches!=1) {
            if (matches) printf("Callbacks: %s: found %" PRIu64 " times, left alone\n",c->name,matches);
            continue;
        }
        if (!thunks && !(thunks=near_image_memory(image_size,16384))) { printf("Callbacks: no thunk memory, mouse uses the stick\n"); return; }
        if (used+320>16384) break; /* a thunk is under 320 bytes */
        unsigned char *t=thunks+used, *f=image+found;
        size_t k=0;
        /* pushfq; push rax rcx rdx r8-r11 rbx; mov rbx, rsp; and rsp, -16; sub rsp, 0xa0 (works at
         * a function's start and inside it) */
        static const unsigned char save[]={0x9c,0x50,0x51,0x52,0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53,0x53,
                                           0x48,0x89,0xe3,0x48,0x83,0xe4,0xf0,0x48,0x81,0xec,0xa0,0x00,0x00,0x00};
        memcpy(t+k,save,sizeof(save)); k+=sizeof(save);
        for (int x=0;x<8;++x) {                                     /* movdqu [rsp+0x20+16x], xmmx */
            const uint32_t disp=0x20u+16u*(uint32_t)x;
            t[k++]=0xf3; t[k++]=0x0f; t[k++]=0x7f; t[k++]=(unsigned char)(0x84|x<<3); t[k++]=0x24;
            memcpy(t+k,&disp,4); k+=4;
        }
        {                                                           /* runtime_guest_ret=[rbx+0x48] (rax, rcx saved) */
            const uintptr_t at=(uintptr_t)&runtime_guest_ret;
            t[k++]=0x48; t[k++]=0x8b; t[k++]=0x43; t[k++]=0x48;     /* mov rax, [rbx+0x48] */
            t[k++]=0x48; t[k++]=0xb9; memcpy(t+k,&at,8); k+=8;      /* movabs rcx, &runtime_guest_ret */
            t[k++]=0x48; t[k++]=0x89; t[k++]=0x01;                  /* mov [rcx], rax */
        }
        if (c->rdx==1) { t[k++]=0x49; t[k++]=0x89; t[k++]=0xd1; }   /* mov r9, rdx */
        t[k++]=0x48; t[k++]=0x89; t[k++]=0xf9;                      /* mov rcx, rdi */
        t[k++]=0x48; t[k++]=0x89; t[k++]=0xf2;                      /* mov rdx, rsi */
        const uintptr_t base=(uintptr_t)image, host=(uintptr_t)c->callback;
        t[k++]=0x49; t[k++]=0xb8; memcpy(t+k,&base,8); k+=8;        /* movabs r8, image */
        if (c->rdx==2) { t[k++]=0x4c; t[k++]=0x8b; t[k++]=0x4b; t[k++]=0x48; } /* mov r9, [rbx+0x48]: the return address */
        if (!c->rdx) { t[k++]=0x4d; t[k++]=0x89; t[k++]=0xf1; }     /* mov r9, r14 */
        if (c->rdx==3) { t[k++]=0x4c; t[k++]=0x8b; t[k++]=0x0b; }  /* mov r9, [rbx]: the guest's rbx */
        if (c->rdx==4) { t[k++]=0x4d; t[k++]=0x89; t[k++]=0xe1; }  /* mov r9, r12 */
        if (c->rdx==6) {                                            /* runtime_guest_r14=r14; then as 5 */
            const uintptr_t at=(uintptr_t)&runtime_guest_r14;
            t[k++]=0x48; t[k++]=0xb8; memcpy(t+k,&at,8); k+=8;      /* movabs rax, &runtime_guest_r14 */
            t[k++]=0x4c; t[k++]=0x89; t[k++]=0x30;                  /* mov [rax], r14 */
        }
        if (c->rdx==5 || c->rdx==6) { t[k++]=0x49; t[k++]=0x89; t[k++]=0xd9; }  /* mov r9, rbx: the saved registers (guest r9 at +0x18, the return address at +0x48) */
        if (c->rdx==2) {                                            /* movabs rax, &runtime_guest_rbp; mov [rax], rbp */
            const uintptr_t at=(uintptr_t)&runtime_guest_rbp;
            t[k++]=0x48; t[k++]=0xb8; memcpy(t+k,&at,8); k+=8;
            t[k++]=0x48; t[k++]=0x89; t[k++]=0x28;
        }
        t[k++]=0x48; t[k++]=0xb8; memcpy(t+k,&host,8); k+=8;        /* movabs rax, callback */
        t[k++]=0xff; t[k++]=0xd0;                                   /* call rax */
        for (int x=0;x<8;++x) {                                     /* movdqu xmmx, [rsp+0x20+16x] */
            const uint32_t disp=0x20u+16u*(uint32_t)x;
            t[k++]=0xf3; t[k++]=0x0f; t[k++]=0x6f; t[k++]=(unsigned char)(0x84|x<<3); t[k++]=0x24;
            memcpy(t+k,&disp,4); k+=4;
        }
        /* mov rsp, rbx; pop rbx r11-r8 rdx rcx rax; popfq */
        static const unsigned char restore[]={0x48,0x89,0xdc,0x5b,0x41,0x5b,0x41,0x5a,0x41,0x59,0x41,0x58,
                                              0x5a,0x59,0x58,0x9d};
        memcpy(t+k,restore,sizeof(restore)); k+=sizeof(restore);
        memcpy(t+k,f,c->moved); k+=c->moved;                       /* the moved instructions */
        int32_t back=(int32_t)((int64_t)(uintptr_t)(f+c->moved)-(int64_t)(uintptr_t)(t+k+5));
        t[k++]=0xe9; memcpy(t+k,&back,4); k+=4;
        int32_t to=(int32_t)((int64_t)(uintptr_t)t-(int64_t)(uintptr_t)(f+5));
        f[0]=0xe9; memcpy(f+1,&to,4); memset(f+5,0x90,c->moved-5);
        used+=(k+15)&~(size_t)15;
        ++installed;
        printf("Callbacks: %s at %#" PRIx64 "\n",c->name,found);
    }
    if (!installed) printf("Callbacks: no mouse camera code found, mouse uses the stick\n");
}
/* GOW3_SCREAM_LOG=1 (UC1 and UC2 1.02 diagnostics): the game's sound engine (SCEA Scream) logs through
 * one function (UC1 0x3c0750, UC2 0x52d570) that drops messages above a verbosity kept in .bss (0) and hands the
 * rest to four optional callbacks, all NULL in the shipped game. The verbosity test is removed
 * and the first callback load reads a host printer. Checked against the exact code bytes. */
static int scream_log_left=30000;
static ABI void scream_log(const char *message, int level) {
    if (scream_log_left<=0) return;
    --scream_log_left;
    size_t n=strlen(message);
    printf("Scream[%d]: %s%s", level, message, n && message[n-1]=='\n' ? "" : "\n");
}
/* The Scream voice/bank errors ("%s(%d) : ...") go through a second printer (0x407ae0): skipped
 * while a flag is set, then handed to the game's output through a pointer at 0x963540. */
static ABI void scream_report(const char *message) {
    if (scream_log_left<=0) return;
    --scream_log_left;
    size_t n=strlen(message);
    printf("Scream report: %s%s", message, n && message[n-1]=='\n' ? "" : "\n");
}
/* Code bytes and offsets per game: UC1 1.02, then UC2 1.02 (same Scream build; UC2 image offsets
 * are vaddr - 0x400000). */
static const struct {
    const char *game;
    unsigned char test[16], slot_load[7], gate[12], out[16];
    uint64_t test_at, slot_load_at, slot, gate_at, out_at;
} scream_sites[]={
    {"UC1",
     {0x48,0x8d,0x05,0xb6,0x0a,0x98,0x00,0x39,0x18,0x0f,0x8c,0xa4,0x00,0x00,0x00,0x48},
     {0x48,0x8b,0x05,0xe7,0x74,0x9b,0x00},
     {0x48,0x8d,0x05,0x84,0x20,0x9b,0x00,0x83,0x38,0x00,0x75,0x63},
     {0x48,0x8d,0x05,0x85,0xb9,0x55,0x00,0x48,0x8d,0x3d,0x3e,0x20,0x9b,0x00,0xff,0x10},
     0x3c07e3, 0x3c083a, 0xd77d28, 0x407b55, 0x407bb4},
    {"UC2",
     {0x48,0x8d,0x05,0x16,0x93,0xf5,0x00,0x39,0x18,0x0f,0x8c,0xa4,0x00,0x00,0x00,0x48},
     {0x48,0x8b,0x05,0x47,0xfd,0xf8,0x00},
     {0x48,0x8d,0x05,0x44,0xaf,0xf8,0x00,0x83,0x38,0x00,0x75,0x63},
     {0x48,0x8d,0x05,0x85,0xf0,0xed,0x00,0x48,0x8d,0x3d,0xfe,0xae,0xf8,0x00,0xff,0x10},
     0x12d603, 0x12d65a, 0x10bd3a8, 0x175325, 0x175384},
};
static void install_scream_log(uint64_t image_size) {
    const size_t count=sizeof(scream_sites)/sizeof(scream_sites[0]);
    size_t g=0;
    for (;g<count;++g) {
        if (scream_sites[g].slot+8<=image_size &&
            !memcmp(image+scream_sites[g].test_at,scream_sites[g].test,sizeof(scream_sites[g].test)) &&
            !memcmp(image+scream_sites[g].slot_load_at,scream_sites[g].slot_load,sizeof(scream_sites[g].slot_load)))
            break;
    }
    if (g==count) {
        printf("Scream log: code not recognised (not UC1 or UC2 1.02), off\n");
        return;
    }
    const uint64_t test_at=scream_sites[g].test_at, slot_load_at=scream_sites[g].slot_load_at;
    /* The engine clears the four slots when it (re)initialises, so the first slot load
     * (mov rax, [rip+slot]) is pointed at a cell of our own instead. */
    unsigned char *cell=allocate(page_size);
    uintptr_t printer=(uintptr_t)scream_log;
    memcpy(cell,&printer,8);
    int64_t rel=(int64_t)(uintptr_t)cell-(int64_t)(uintptr_t)(image+slot_load_at+7);
    if (rel!=(int32_t)rel) { printf("Scream log: no cell within 2 GiB of the image, off\n"); return; }
    int32_t rel32=(int32_t)rel;
    memset(image+test_at+9,0x90,6); /* jl past the printing: every level prints */
    memcpy(image+slot_load_at+3,&rel32,4);
    printf("Scream log: on for %s (sound engine messages print as Scream[level])\n",scream_sites[g].game);
    const uint64_t gate_at=scream_sites[g].gate_at, out_at=scream_sites[g].out_at;
    if (memcmp(image+gate_at,scream_sites[g].gate,sizeof(scream_sites[g].gate)) ||
        memcmp(image+out_at,scream_sites[g].out,sizeof(scream_sites[g].out))) {
        printf("Scream report: code not recognised, off\n");
        return;
    }
    uintptr_t reporter=(uintptr_t)scream_report;
    memcpy(cell+8,&reporter,8);
    rel=(int64_t)(uintptr_t)(cell+8)-(int64_t)(uintptr_t)(image+out_at+7);
    if (rel!=(int32_t)rel) return;
    rel32=(int32_t)rel;
    memset(image+gate_at+10,0x90,2); /* jne past the report while it is switched off */
    memcpy(image+out_at+3,&rel32,4);
    printf("Scream report: on\n");
}
/* UC2 1.02 state-script trace (GOW3_DC_TRACE=1, off by default): the natives the DC scripts
 * call (go, send-event, animate, ...) are registered at init with `lea rax,[rip+func]`; each
 * lea is pointed at a stub that prints the call, then jumps on to the native. A native gets
 * edi = argc, rsi = 8-byte argument slots. Symbols print as hex (resolve them offline). */
static const struct { const char *name; uint32_t site, func; } dc_trace_natives[]={
    {"go",0xe3ecd3,0xe44290}, {"send-event",0x8c1786,0x8c1800}, {"animate",0x796e63,0x7969a0},
    {"wait-animate",0x796ec7,0x797ab0}, {"wait",0xe3f4a3,0xe41270}, {"wait-for",0xe3f827,0xe3fb60},
    {"signal",0xe3f6fb,0xe40220}, {"camera-animation-enable",0xdf7f14,0xe033b0},
    {"camera-enable",0xdf5610,0xe0e5e0}, {"fall-to-death",0xd04d0b,0xd0c3d0},
    {"kill-player",0xd04bdf,0xd0c5f0}, {"task-failed!",0xdfa494,0xdfe8f0},
    {"player-disable-controls",0xd059ef,0xd09610}, {"camera-disable-all",0xdf57a0,0xe0e290},
    {"do-rumble-ptr",0xdf4544,0xe105b0}, {"get-animation-phase",0x797057,0x7976a0},
    {"print-float",0xdfab38,0xdfdef0}, {"print-string",0xdfac00,0xdfdea0},
    {"player-in-region?",0xd04e37,0xd0bc10}, {"spawn-entity",0xdf68d0,0xdf3900},
};
static int dc_trace_left=300000;
static double dc_trace_seconds(void) {
#ifdef _WIN32
    static LARGE_INTEGER frequency, start;
    LARGE_INTEGER now;
    if (!frequency.QuadPart) { QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&start); }
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart-start.QuadPart)/(double)frequency.QuadPart;
#else
    return 0.0;
#endif
}
static ABI void dc_trace(int argc, const uint64_t *args, int id) {
    if (dc_trace_left<=0) return;
    --dc_trace_left;
    char line[256];
    int n=snprintf(line,sizeof(line),"DC t=%.3f %s argc=%d",dc_trace_seconds(),dc_trace_natives[id].name,argc);
    for (int i=0; args && i<argc && i<6 && n>0 && n<(int)sizeof(line); ++i)
        n+=snprintf(line+n,sizeof(line)-n," %" PRIx64,args[i]);
    printf("%s\n",line);
}
static void install_dc_trace(uint64_t image_size) {
    const size_t count=sizeof(dc_trace_natives)/sizeof(dc_trace_natives[0]), stub=64;
    for (size_t i=0;i<count;++i) {
        uint64_t at=dc_trace_natives[i].site-0x400000ull;
        int32_t disp;
        if (at+7>image_size || image[at]!=0x48 || image[at+1]!=0x8d || image[at+2]!=0x05) goto unknown;
        memcpy(&disp,image+at+3,4);
        if (at+7+(int64_t)disp!=dc_trace_natives[i].func-0x400000ull) goto unknown;
    }
    unsigned char *page=allocate(round_page(count*stub));
    uintptr_t helper=(uintptr_t)dc_trace;
    for (size_t i=0;i<count;++i) {
        unsigned char *s=page+i*stub;
        uintptr_t func=(uintptr_t)(image+dc_trace_natives[i].func-0x400000ull);
        uint32_t id=(uint32_t)i;
        /* push rax,rdi,rsi,rdx,rcx,r8,r9 (7 pushes: the call below sees an aligned stack) */
        static const unsigned char save[]={0x50,0x57,0x56,0x52,0x51,0x41,0x50,0x41,0x51};
        static const unsigned char restore[]={0x41,0x59,0x41,0x58,0x59,0x5a,0x5e,0x5f,0x58};
        unsigned char *p=s;
        memcpy(p,save,sizeof(save)); p+=sizeof(save);
        *p++=0xba; memcpy(p,&id,4); p+=4;                        /* mov edx, id */
        *p++=0x48; *p++=0xb8; memcpy(p,&helper,8); p+=8;          /* mov rax, dc_trace */
        *p++=0xff; *p++=0xd0;                                     /* call rax */
        memcpy(p,restore,sizeof(restore)); p+=sizeof(restore);
        *p++=0xff; *p++=0x25; memset(p,0,4); p+=4;                /* jmp [rip+0] */
        memcpy(p,&func,8);
    }
    for (size_t i=0;i<count;++i) {
        uint64_t at=dc_trace_natives[i].site-0x400000ull;
        int64_t rel=(int64_t)(uintptr_t)(page+i*stub)-(int64_t)(uintptr_t)(image+at+7);
        if (rel!=(int32_t)rel) { printf("DC trace: no stub page within 2 GiB of the image, off\n"); return; }
    }
    protect(page,round_page(count*stub),5);
    for (size_t i=0;i<count;++i) {
        uint64_t at=dc_trace_natives[i].site-0x400000ull;
        int32_t rel32=(int32_t)((int64_t)(uintptr_t)(page+i*stub)-(int64_t)(uintptr_t)(image+at+7));
        memcpy(image+at+3,&rel32,4);
    }
    printf("DC trace: on (%u natives)\n",(unsigned)count);
    return;
unknown:
    printf("DC trace: code not recognised (not UC2 1.02), off\n");
}
#ifdef _WIN32
/* UC2 1.02 player position log (GOW3_PLAYER_POS=1, off by default): a thread reads the player
 * handle the game keeps at 0x155bd20 (player-in-region? loads it at 0xd0bc50) every few ms and
 * prints the player object's locators at +0xf0 and +0x110 (see get-object-position, 0xe12390)
 * when they change. ReadProcessMemory never faults: a freed object is only a failed read. */
static uintptr_t player_pos_handle;
static int player_pos_read(uintptr_t at, void *out, size_t n) {
    SIZE_T got=0;
    return ReadProcessMemory(GetCurrentProcess(),(LPCVOID)at,out,n,&got) && got==n;
}
static DWORD WINAPI player_pos_thread(LPVOID unused) {
    (void)unused;
    float last[8]={0};
    int left=300000;
    while (left>0) {
        Sleep(2);
        uint64_t holder=0, object=0;
        uint32_t id=0, object_id=0;
        float v[8];
        if (!player_pos_read(player_pos_handle,&holder,8) || !holder ||
            !player_pos_read(player_pos_handle+8,&id,4) || !id) continue;
        if (!player_pos_read(holder,&object,8) || !object ||
            !player_pos_read(object+0x44,&object_id,4) || object_id!=id) continue;
        if (!player_pos_read(object+0xf0,v,16) || !player_pos_read(object+0x110,v+4,16)) continue;
        if (!memcmp(v,last,sizeof(v))) continue;
        memcpy(last,v,sizeof(v));
        --left;
        printf("POS t=%.3f obj=%" PRIx64 " a=%.3f %.3f %.3f b=%.3f %.3f %.3f\n",dc_trace_seconds(),object,
               v[0],v[1],v[2],v[4],v[5],v[6]);
    }
    return 0;
}
static void install_player_pos(uint64_t image_size) {
    const uint64_t at=0xd0bc50-0x400000ull, handle=0x155bd20-0x400000ull;
    int32_t disp;
    if (handle+16>image_size || image[at]!=0x48 || image[at+1]!=0x8d || image[at+2]!=0x0d) goto unknown;
    memcpy(&disp,image+at+3,4);
    if (at+7+(int64_t)disp!=handle) goto unknown;
    player_pos_handle=(uintptr_t)(image+handle);
    dc_trace_seconds();
    HANDLE thread=CreateThread(NULL,0,player_pos_thread,NULL,0,NULL);
    if (!thread) { printf("Player position log: no thread, off\n"); return; }
    CloseHandle(thread);
    printf("Player position log: on\n");
    return;
unknown:
    printf("Player position log: code not recognised (not UC2 1.02), off\n");
}
/* uc2 (Drake slow with READBACK_ASYNC): GOW3_DT_SCAN=1 looks for the game's frame time in its
 * writable image data. In rounds of 20 s: 10 s of samples (every 200 ms) of every aligned float
 * and double, then the slots that always held 0.004..0.05 and changed most are printed
 * ("DT cand") and the top 8 watched every 1 ms for 10 s ("DT t=... <addr>=<value>" on change). */
typedef struct { uint64_t start, size; } DtRange;
static DtRange dt_ranges[16];
static int dt_range_count;
static int dt_read(uint64_t at, void *out, size_t n) {
    SIZE_T got=0;
    return ReadProcessMemory(GetCurrentProcess(),(LPCVOID)at,out,n,&got) && got==n;
}
typedef struct { uint64_t addr; int dbl; double mean, lo, hi; unsigned changes; } DtCand;
static DWORD WINAPI dt_scan_thread(LPVOID unused) {
    (void)unused;
    uint64_t total=0;
    for (int r=0;r<dt_range_count;++r) total+=dt_ranges[r].size;
    const size_t slots=(size_t)(total/4);
    unsigned char *in_f=calloc(slots,1), *ch_f=calloc(slots,1), *in_d=calloc(slots/2+1,1), *ch_d=calloc(slots/2+1,1);
    float *sum_f=calloc(slots,sizeof(float)), *last_f=calloc(slots,sizeof(float));
    double *sum_d=calloc(slots/2+1,sizeof(double)), *last_d=calloc(slots/2+1,sizeof(double));
    unsigned char *buf=malloc(1<<16);
    if (!in_f||!ch_f||!in_d||!ch_d||!sum_f||!last_f||!sum_d||!last_d||!buf) { printf("DT scan: out of memory\n"); return 0; }
    for (int round=0;round<30;++round) {
        memset(in_f,0,slots); memset(ch_f,0,slots); memset(in_d,0,slots/2+1); memset(ch_d,0,slots/2+1);
        memset(sum_f,0,slots*sizeof(float)); memset(sum_d,0,(slots/2+1)*sizeof(double));
        const int samples=50;
        for (int k=0;k<samples;++k) {
            size_t base=0;
            for (int r=0;r<dt_range_count;++r) {
                for (uint64_t off=0;off<dt_ranges[r].size;off+=1<<16) {
                    const size_t n=(size_t)(dt_ranges[r].size-off<(1<<16)?dt_ranges[r].size-off:(1<<16));
                    if (!dt_read(dt_ranges[r].start+off,buf,n)) continue;
                    for (size_t i=0;i+4<=n;i+=4) {
                        const size_t f=base+(size_t)(off+i)/4;
                        float v; memcpy(&v,buf+i,4);
                        if (v>=0.004f && v<=0.05f && in_f[f]<255) { ++in_f[f]; sum_f[f]+=v; }
                        if (k && v!=last_f[f] && ch_f[f]<255) ++ch_f[f];
                        last_f[f]=v;
                        if (!(i&7) && i+8<=n) {
                            const size_t d=f/2;
                            double w; memcpy(&w,buf+i,8);
                            if (w>=0.004 && w<=0.05 && in_d[d]<255) { ++in_d[d]; sum_d[d]+=w; }
                            if (k && w!=last_d[d] && ch_d[d]<255) ++ch_d[d];
                            last_d[d]=w;
                        }
                    }
                }
                base+=(size_t)(dt_ranges[r].size/4);
            }
            Sleep(200);
        }
        DtCand top[16]; int ntop=0;
        size_t base=0;
        for (int r=0;r<dt_range_count;++r) {
            const size_t count=(size_t)(dt_ranges[r].size/4);
            for (size_t j=0;j<count;++j) {
                const size_t f=base+j;
                for (int dbl=0;dbl<2;++dbl) {
                    if (dbl && (j&1)) continue;
                    const unsigned in=dbl?in_d[f/2]:in_f[f], ch=dbl?ch_d[f/2]:ch_f[f];
                    if (in<samples || ch<10) continue;
                    DtCand c={dt_ranges[r].start+j*4,dbl,(dbl?sum_d[f/2]:sum_f[f])/samples,0,0,ch};
                    if (ntop==16 && top[15].changes>=ch) continue;
                    const int at=ntop<16?ntop++:15;
                    top[at]=c;
                    for (int q=at;q>0 && top[q].changes>top[q-1].changes;--q) { DtCand t=top[q]; top[q]=top[q-1]; top[q-1]=t; }
                }
            }
            base+=count;
        }
        printf("DT scan round %d: %d candidates\n",round,ntop);
        for (int q=0;q<ntop;++q)
            printf("DT cand %#" PRIx64 " %s mean %.5f changes %u/%d\n",top[q].addr-(uint64_t)(uintptr_t)image+0x400000ull,
                   top[q].dbl?"double":"float",top[q].mean,top[q].changes,samples-1);
        const int watch=ntop<8?ntop:8;
        double last[8]={0};
        const double t_end=dc_trace_seconds()+10.0;
        while (watch && dc_trace_seconds()<t_end) {
            Sleep(1);
            for (int q=0;q<watch;++q) {
                double v=0;
                if (top[q].dbl) { if (!dt_read(top[q].addr,&v,8)) continue; }
                else { float x; if (!dt_read(top[q].addr,&x,4)) continue; v=x; }
                if (v==last[q]) continue;
                last[q]=v;
                printf("DT t=%.4f %#" PRIx64 "=%.6f\n",dc_trace_seconds(),top[q].addr-(uint64_t)(uintptr_t)image+0x400000ull,v);
            }
        }
    }
    return 0;
}
static void install_dt_scan(const Segment *segments, uint64_t count) {
    for (uint64_t s=0;s<count && dt_range_count<16;++s) {
        if (!(segments[s].flags&2) || (segments[s].flags&1) || !segments[s].size) continue;
        dt_ranges[dt_range_count].start=(uint64_t)(uintptr_t)(image+segments[s].address);
        dt_ranges[dt_range_count].size=(segments[s].size<(48ull<<20)?segments[s].size:(48ull<<20))&~7ull;
        printf("DT scan: segment vaddr %#" PRIx64 " size %#" PRIx64 "\n",segments[s].address+0x400000ull,segments[s].size);
        ++dt_range_count;
    }
    dc_trace_seconds();
    HANDLE thread=dt_range_count?CreateThread(NULL,0,dt_scan_thread,NULL,0,NULL):NULL;
    if (!thread) { printf("DT scan: off (no writable segment or thread)\n"); return; }
    CloseHandle(thread);
    printf("DT scan: on\n");
}
#endif
/* Guest code reads its TCB with `mov rax, gs:[0]` (link_*.py rewrote fs:[0]); on Windows
 * the displacement becomes the TEB TLS slot that holds the guest TCB (runtime_thread.c). */
uint32_t runtime_thread_tcb_offset(void);
static uint64_t patch_tcb_loads(const Segment *segments, uint64_t count) {
    static const unsigned char load[]={0x65,0x48,0x8b,0x04,0x25,0,0,0,0};
    uint32_t displacement=runtime_thread_tcb_offset();
    uint64_t patched=0;
    for (uint64_t s=0;s<count;++s) {
        if (!(segments[s].flags&1) || segments[s].size<sizeof(load)) continue;
        unsigned char *at=image+segments[s].address, *end=at+segments[s].size-sizeof(load);
        for (; at<=end; ++at)
            if (at[0]==0x65 && !memcmp(at,load,sizeof(load))) { memcpy(at+5,&displacement,4); ++patched; at+=sizeof(load)-1; }
    }
    return patched;
}
#endif
#ifndef _WIN32
static void fault(int sig, siginfo_t *info, void *context) {
    /* GPU page tracking (write-protected guest pages) is resolved first, except for a speculative
     * read, which gives up instead of waiting for the GPU thread (see vectored_fault). */
    if (gpu_enabled && sig == SIGSEGV && !runtime_fault_recover && gow3gpu_handle_fault(context, info->si_addr)) return;
    /* A speculative guest memory read (runtime_memory.c) failed: resume its recovery point. */
    if ((sig == SIGSEGV || sig == SIGBUS) && runtime_fault_recover) {
        sigjmp_buf *recover = runtime_fault_recover;
        runtime_fault_recover = NULL;
        sigset_t unblock;
        sigemptyset(&unblock);
        sigaddset(&unblock, sig);
        pthread_sigmask(SIG_UNBLOCK, &unblock, NULL);
        siglongjmp(*recover, 1);
    }
    /* The process is terminating: dladdr/snprintf are acceptable here. */
    ucontext_t *uc = context;
    uintptr_t rip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    char line[512];
    Dl_info where;
    if (rip - (uintptr_t)image < 0x10000000)
        snprintf(line, sizeof(line), "Guest fault (signal %d) at guest offset 0x%lx, address %p\n",
                 sig, (unsigned long)(rip - (uintptr_t)image), info->si_addr);
    else if (dladdr((void *)rip, &where) && where.dli_fname)
        snprintf(line, sizeof(line), "Host fault (signal %d) in %s+0x%lx (%s), address %p\n", sig, where.dli_fname,
                 (unsigned long)(rip - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?", info->si_addr);
    else
        snprintf(line, sizeof(line), "Fault (signal %d) at RIP %p, address %p\n", sig, (void *)rip, info->si_addr);
    { ssize_t written_=write(2, line, strlen(line)); (void)written_; }
    if (gpu_enabled) gow3gpu_dump_guest_writes(context);
    /* Host call chain (frames with unwind info; guest frames end it). */
    void *frames[32];
    int depth = backtrace(frames, 32);
    for (int i = 2; i < depth; ++i) {
        if (dladdr(frames[i], &where) && where.dli_fname)
            snprintf(line, sizeof(line), "  #%d %s+0x%lx (%s)\n", i, where.dli_fname,
                     (unsigned long)((uintptr_t)frames[i] - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?");
        else
            snprintf(line, sizeof(line), "  #%d %p\n", i, frames[i]);
        ssize_t written_=write(2, line, strlen(line)); (void)written_;
    }
    _exit(128 + sig);
}
#endif
#ifndef _WIN32
/* Watchdog: dump RIP and the rbp frame chain of every thread (guest offsets
 * when inside the image). Reads use process_vm_readv so bad frames cannot fault. */
static uintptr_t exe_base;
static void write_hex(char *out, uint64_t v) {
    const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) out[i] = digits[(v >> (60 - i * 4)) & 15];
}
static void dump_frames(ucontext_t *uc) {
    char line[] = "  tid=0000000000000000 rip=0000000000000000 image-relative=0000000000000000 host-relative=0000000000000000\n";
    uintptr_t rip=(uintptr_t)uc->uc_mcontext.gregs[REG_RIP], rbp=(uintptr_t)uc->uc_mcontext.gregs[REG_RBP];
    uint64_t tid=(uint64_t)gettid();
    /* First argument register: the lock address when a thread waits on a futex. */
    char arg[]="  tid=0000000000000000 rdi=0000000000000000\n";
    write_hex(arg+6,tid); write_hex(arg+27,(uint64_t)uc->uc_mcontext.gregs[REG_RDI]);
    { ssize_t written_=write(2,arg,sizeof(arg)-1); (void)written_; }
    for (int depth=0; depth<24; ++depth) {
        write_hex(line+6,tid); write_hex(line+27,rip); write_hex(line+59,rip-(uintptr_t)image); write_hex(line+90,rip-exe_base);
        { ssize_t written_=write(2,line,sizeof(line)-1); (void)written_; }
        uintptr_t frame[2];
        struct iovec local={frame,sizeof(frame)}, remote={(void *)rbp,sizeof(frame)};
        if (!rbp || process_vm_readv(getpid(),&local,1,&remote,1,0)!=(ssize_t)sizeof(frame)) break;
        if (frame[0]<=rbp) break;
        rbp=frame[0]; rip=frame[1];
    }
}
static void thread_dump(int sig, siginfo_t *info, void *context) { (void)sig; (void)info; dump_frames(context); }
static void watchdog(int sig, siginfo_t *info, void *context) {
    (void)info;
    const char head[]="STOP: watchdog timeout; thread stacks:\n";
    { ssize_t written_=write(2,head,sizeof(head)-1); (void)written_; }
    dump_frames(context);
    int dir=open("/proc/self/task",O_RDONLY|O_DIRECTORY);
    char buffer[4096];
    long n;
    pid_t self=gettid();
    while (dir>=0 && (n=syscall(SYS_getdents64,dir,buffer,sizeof(buffer)))>0)
        for (long at=0; at<n;) {
            struct { uint64_t ino; int64_t off; unsigned short reclen; unsigned char type; char name[]; } *d=(void *)(buffer+at);
            pid_t tid=(pid_t)strtol(d->name,NULL,10);
            if (tid>0 && tid!=self) { syscall(SYS_tgkill,getpid(),tid,SIGUSR2); usleep(20000); }
            at+=d->reclen;
        }
    usleep(100000);
    _exit(128 + sig);
}
#endif
/* param.sfo lookup: string or integer value of key, 0 when absent. */
static int sfo_value(const char *path, const char *key, char *text, size_t text_size, uint32_t *number) {
    FILE *f=fopen(path,"rb");
    if (!f) return 0;
    unsigned char data[65536];
    size_t n=fread(data,1,sizeof(data),f); fclose(f);
    if (n<20 || memcmp(data,"\0PSF",4)) return 0;
    uint32_t keys, values, count;
    memcpy(&keys,data+8,4); memcpy(&values,data+12,4); memcpy(&count,data+16,4);
    for (uint32_t i=0;i<count && 20+i*16+16<=n;++i) {
        const unsigned char *e=data+20+i*16;
        uint16_t key_offset, format; uint32_t length, offset;
        memcpy(&key_offset,e,2); memcpy(&format,e+2,2); memcpy(&length,e+4,4); memcpy(&offset,e+12,4);
        if (keys+key_offset>=n || values+offset+length>n || strcmp((const char *)data+keys+key_offset,key)) continue;
        if (format==0x0404 && number && length>=4) { memcpy(number,data+values+offset,4); return 1; }
        if (text && text_size) {
            size_t copy=length<text_size-1 ? length : text_size-1;
            memcpy(text,data+values+offset,copy); text[copy]=0;
        }
        return 1;
    }
    return 0;
}
static int mapped(Segment *segments, uint64_t count, uint64_t address, uint64_t bytes) {
    for (uint64_t i = 0; i < count; ++i)
        if (address >= segments[i].address && bytes <= segments[i].size &&
            address - segments[i].address <= segments[i].size - bytes) return 1;
    return 0;
}
extern ABI float gow3_orbs_gain(void *player, float requested);
extern void gow3_orbs_status(int supported);
extern uintptr_t gow3_cheat_flag(unsigned id);
extern uintptr_t gow3_actor_slots_address(void);
extern void gow3_actor_status(int supported);
extern void gow3_cheat_status(unsigned id);
extern uintptr_t gow3_damage_multiplier(int taken);
extern uintptr_t gow3_orb_pickup_multiplier(unsigned kind);
extern void gow3_stat_multipliers_status(int damage, int orb_pickup);

/* The hooks below find their sites by signature (src/signature.h), so they work in any region
 * and version of the game whose code matches; a site that is not found leaves its feature off. */
static void install_cheats(Segment *segments, uint64_t count, uint64_t size) {
    const char *names[] = {"health", "magic", "item meter", "Rage of Sparta", "red orbs",
                           "Very Hard unlock", "infinite double jumps"};
    uint64_t at[GOW3_CHEAT_HOOK_COUNT];
    for (unsigned i = 0; i < GOW3_CHEAT_HOOK_COUNT; ++i)
        at[i] = gow3_signature_find(image, size, &gow3_cheat_hooks[i].site);
    for (unsigned id = 0; id < GOW3_CHEAT_COUNT; ++id) {
        int valid = 1;
        for (unsigned i = 0; i < GOW3_CHEAT_HOOK_COUNT; ++i) {
            const Gow3CheatHook *hook = &gow3_cheat_hooks[i];
            if (hook->id != id) continue;
            if (at[i] == UINT64_MAX || !mapped(segments, count, at[i], hook->length) ||
                !gow3_cheat_signature_match(hook, image + at[i], size - at[i])) valid = 0;
        }
        if (!valid) {
            printf("Cheats: %s unavailable (code not found in this eboot.bin)\n", names[id]);
            continue;
        }
        unsigned char *stubs = allocate(page_size);
        int32_t relative[GOW3_CHEAT_HOOK_COUNT] = {0};
        for (unsigned i = 0; i < GOW3_CHEAT_HOOK_COUNT; ++i) {
            const Gow3CheatHook *hook = &gow3_cheat_hooks[i];
            if (hook->id != id) continue;
            unsigned char *stub = stubs + i * 256;
            int64_t delta = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + at[i] + 5);
            if (delta < INT32_MIN || delta > INT32_MAX) { valid = 0; break; }
            relative[i] = (int32_t)delta;
            gow3_cheat_build_stub(stub, hook, gow3_cheat_flag(id),
                                 (uintptr_t)(image + at[i] + hook->length));
        }
        if (!valid) {
            printf("Cheats: %s unavailable (hook outside relative-jump range)\n", names[id]);
            continue;
        }
        protect(stubs, page_size, 5);
        for (unsigned i = 0; i < GOW3_CHEAT_HOOK_COUNT; ++i) {
            const Gow3CheatHook *hook = &gow3_cheat_hooks[i];
            if (hook->id != id) continue;
            image[at[i]] = 0xe9;
            memcpy(image + at[i] + 1, &relative[i], 4);
            memset(image + at[i] + 5, 0x90, hook->length - 5);
        }
        gow3_cheat_status(id);
        printf("Cheats: %s ready (live toggle, initially disabled unless saved)\n", names[id]);
    }
}

/* Damage hook for the enemy health bar (src/actor_hook.h). Always
 * installed when the code matches: it only records values; the overlay decides what to show. */
static void install_actor_watch(Segment *segments, uint64_t count, uint64_t size) {
    const uint64_t at = gow3_signature_find(image, size, &gow3_actor_site);
    if (at == UINT64_MAX || !mapped(segments, count, at, GOW3_ACTOR_HOOK_LENGTH) ||
        !gow3_actor_signature_match(image + at, size - at)) {
        printf("Health display: unavailable (code not found in this eboot.bin)\n");
        return;
    }
    unsigned char *stub = allocate(page_size);
    const int64_t delta = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + at + 5);
    if (delta < INT32_MIN || delta > INT32_MAX) {
        printf("Health display: unavailable (hook outside relative-jump range)\n");
        return;
    }
    gow3_actor_build_stub(stub, gow3_actor_slots_address(), (uintptr_t)(image + at + GOW3_ACTOR_HOOK_LENGTH));
    protect(stub, page_size, 5);
    const int32_t relative = (int32_t)delta;
    image[at] = 0xe9;
    memcpy(image + at + 1, &relative, 4);
    memset(image + at + 5, 0x90, GOW3_ACTOR_HOOK_LENGTH - 5);
    gow3_actor_status(1);
    printf("Health display: damage hook ready (enemy health bar in the menu)\n");
}

static void install_orb_multiplier(Segment *segments, uint64_t count, uint64_t size) {
    uint64_t calls[3];
    const uint64_t setter_at = gow3_orb_find(image, size, calls);
    if (setter_at == UINT64_MAX) {
        printf("Red orbs: multiplier unavailable (code not found in this eboot.bin)\n");
        return;
    }
    for (unsigned i = 0; i < 3; ++i) {
        if (!mapped(segments, count, calls[i], 5)) return;
    }
    /* SysV call sites: preserve the receiver across the helper, then tail-call the original
     * setter. Its cap and upgrade notifications remain in the game's own code. */
    unsigned char *stub = allocate(page_size);
    uintptr_t helper = (uintptr_t)gow3_orbs_gain, setter = (uintptr_t)(image + setter_at);
    unsigned char code[26];
    gow3_orb_build_stub(code, helper, setter);
    int32_t displacements[3];
    for (unsigned i = 0; i < 3; ++i) {
        int64_t delta = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + calls[i] + 5);
        if (delta < INT32_MIN || delta > INT32_MAX) {
            printf("Red orbs: multiplier unavailable (hook outside relative-call range)\n");
            return;
        }
        displacements[i] = (int32_t)delta;
    }
    memcpy(stub, code, sizeof(code));
    protect(stub, page_size, 5);
    for (unsigned i = 0; i < 3; ++i)
        memcpy(image + calls[i] + 1, &displacements[i], 4);
    gow3_orbs_status(1);
    printf("Red orbs: live multiplier ready (3 validated gain paths, 0.1x to 100x)\n");
}
/* Writes a jump from image+at to `stub`; the rest of the replaced bytes become nops (the stub
 * replays them). 0 when the stub is out of relative-jump range. */
static int install_jump(uint64_t at, unsigned length, unsigned char *stub) {
    const int64_t delta = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + at + 5);
    if (delta < INT32_MIN || delta > INT32_MAX) return 0;
    const int32_t relative = (int32_t)delta;
    image[at] = 0xe9;
    memcpy(image + at + 1, &relative, 4);
    memset(image + at + 5, 0x90, length - 5);
    return 1;
}

/* Damage dealt/taken and green/blue/gold orb multipliers (src/damage_hook.h, src/orb_hook.h):
 * the stubs read the menu's floats directly, so changes apply on the next hit or pickup. */
static void install_stat_multipliers(Segment *segments, uint64_t count, uint64_t size) {
    const uint64_t damage_at = gow3_signature_find(image, size, &gow3_damage_site);
    const uint64_t pickup_at = gow3_signature_find(image, size, &gow3_orb_pickup_site);
    int damage = damage_at != UINT64_MAX && mapped(segments, count, damage_at, GOW3_DAMAGE_HOOK_LENGTH) &&
                 gow3_damage_signature_match(image + damage_at, size - damage_at);
    int pickup = pickup_at != UINT64_MAX && mapped(segments, count, pickup_at, GOW3_ORB_PICKUP_LENGTH) &&
                 gow3_orb_pickup_signature_match(image + pickup_at, size - pickup_at);
    unsigned char *stubs = damage || pickup ? allocate(page_size) : NULL;
    if (damage) {
        gow3_damage_build_stub(stubs, gow3_damage_multiplier(1), gow3_damage_multiplier(0),
                               (uintptr_t)(image + damage_at + GOW3_DAMAGE_HOOK_LENGTH));
    }
    if (pickup) {
        gow3_orb_pickup_build_stub(stubs + 256, gow3_orb_pickup_multiplier(0), gow3_orb_pickup_multiplier(1),
                                   gow3_orb_pickup_multiplier(2),
                                   (uintptr_t)(image + pickup_at + GOW3_ORB_PICKUP_LENGTH));
    }
    if (stubs) protect(stubs, page_size, 5);
    damage = damage && install_jump(damage_at, GOW3_DAMAGE_HOOK_LENGTH, stubs);
    pickup = pickup && install_jump(pickup_at, GOW3_ORB_PICKUP_LENGTH, stubs + 256);
    gow3_stat_multipliers_status(damage, pickup);
    printf("Multipliers: damage %s, green/blue/gold orbs %s\n", damage ? "ready" : "unavailable",
           pickup ? "ready" : "unavailable");
}
/* G3PATCH2 (patches.py): the patches' image base, then byte writes at image offsets, applied
 * after relocation. A write may replace a whole base-relative pointer slot (60/90 FPS++ swap
 * function pointers): the patch holds the address at the patches' base, rebased here. */
static void apply_patches(const char *path, Segment *segments, uint64_t ns, const Reloc *relocs, uint64_t nr) {
    FILE *f=fopen(path,"rb");
    char magic[8];
    if (!f || fread(magic,1,8,f)!=8 || memcmp(magic,"G3PATCH2",8)) fail("invalid patch file");
    uint64_t base=read64(f), count=read64(f), bytes=0, rebased=0;
    unsigned char data[4096];
    for (uint64_t i=0;i<count;++i) {
        uint64_t offset=read64(f), length=read64(f);
        if (!length || length>sizeof(data) || !mapped(segments,ns,offset,length) || fread(data,1,length,f)!=length)
            fail("bad patch entry");
        uint64_t slots[sizeof(data)/8]; size_t nslots=0;
        for (uint64_t r=0;r<nr;++r) {
            if (!(relocs[r].target<offset+length && offset<relocs[r].target+8)) continue;
            uint64_t target=relocs[r].target, value;
            if (relocs[r].kind || target<offset || target+8>offset+length) fail("patch overlaps a relocation");
            memcpy(&value,data+(target-offset),8);
            if (value<base || !mapped(segments,ns,value-base,1)) fail("patch writes a pointer outside the image");
            slots[nslots++]=target;
        }
        memcpy(image+offset,data,length);
        for (size_t s=0;s<nslots;++s) {
            uint64_t value;
            memcpy(&value,image+slots[s],8);
            value=(uint64_t)(uintptr_t)image+(value-base);
            memcpy(image+slots[s],&value,8);
        }
        rebased+=nslots;
        bytes+=length;
    }
    if (fgetc(f)!=EOF) fail("trailing data in patch file");
    fclose(f);
    printf("Patches: %" PRIu64 " writes, %" PRIu64 " bytes applied, %" PRIu64 " pointers rebased\n",count,bytes,rebased);
}
/* Restarts the game through run.sh (the settings menu: a new render resolution is a patch
 * applied at start). Descriptors are closed first so the old GPU device and its memory are
 * released before the new process opens its own. */
void runtime_restart(void) {
    fflush(NULL);
#ifdef _WIN32
    /* run.py sets GOW3_RESTART_COMMAND: the launch command line, started again (new patches). */
    const char *command=getenv("GOW3_RESTART_COMMAND");
    puts("Runtime: restarting through run.py");
    if (!command || !*command) { fputs("runtime_restart: GOW3_RESTART_COMMAND is not set\n",stderr); _exit(1); }
    char *line=_strdup(command);
    /* Same output handles: a launcher reading the game's log keeps reading the new launch. */
    STARTUPINFOA startup={.cb=sizeof(startup),.dwFlags=STARTF_USESTDHANDLES,
                          .hStdInput=GetStdHandle(STD_INPUT_HANDLE),.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE),
                          .hStdError=GetStdHandle(STD_ERROR_HANDLE)};
    PROCESS_INFORMATION process;
    if (!line || !CreateProcessA(NULL,line,NULL,NULL,TRUE,0,NULL,NULL,&startup,&process)) {
        fprintf(stderr,"runtime_restart: CreateProcess failed (%lu)\n",GetLastError()); _exit(1);
    }
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    /* The new launch waits for this process's GPU device to go away (run.py --after). */
    TerminateProcess(GetCurrentProcess(),0);
#else
    const char *command = getenv("GOW3_RESTART_COMMAND");
    syscall(SYS_close_range, 3u, ~0u, 0u);
    if (command && *command) {
        puts("Runtime: restarting through GOW3_RESTART_COMMAND");
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
    } else {
        puts("Runtime: restarting through run.sh");
        execlp("bash", "bash", "run.sh", (char *)NULL);
    }
    perror("runtime_restart: exec");
    _exit(1);
#endif
}

/* ucndc (GOW3_IMPORT_COUNT=1, diagnostic): every resolved function import goes through a thunk
   that counts its calls; a reporter prints the most called imports every 10 s, to see which
   kernel calls the game's busy threads (NdJobWorkerThread) make while they wait. The thunk uses
   only r11 (not an argument register; rax carries the vector count of variadic calls). */
#include <pthread.h>
static uint64_t *import_calls; /* one counter per import, 64 bytes apart */
#define IMPORT_CALL_STRIDE 8
static void *import_count_report(void *unused) {
    (void)unused;
    uint64_t *last=calloc(import_count ? import_count : 1,sizeof(*last));
    uint64_t *order=calloc(import_count ? import_count : 1,sizeof(*order));
    if (!last || !order) return NULL;
    for (;;) {
#ifdef _WIN32
        Sleep(10000);
#else
        sleep(10);
#endif
        uint64_t total=0, n=0;
        for (uint64_t i=0;i<import_count;++i) {
            uint64_t now=__atomic_load_n(&import_calls[i*IMPORT_CALL_STRIDE],__ATOMIC_RELAXED);
            uint64_t delta=now-last[i];
            last[i]=now;
            order[i]=delta;
            total+=delta;
            if (delta) ++n;
        }
        printf("Import calls (10 s): %" PRIu64 " calls in %" PRIu64 " imports; top:\n", total, n);
        for (int k=0;k<15;++k) {
            uint64_t best=0, at=import_count;
            for (uint64_t i=0;i<import_count;++i) if (order[i]>best) { best=order[i]; at=i; }
            if (at==import_count) break;
            printf("  %12" PRIu64 "  %s  [%s]\n", best, runtime_import_name(names[at]), names[at]);
            order[at]=0;
        }
        fflush(stdout);
    }
    return NULL;
}

#ifdef _WIN32
/* ucndc (GOW3_RIP_SAMPLE=1, diagnostic): every ~1 ms suspends each guest thread whose name
   contains GOW3_RIP_SAMPLE_FILTER (default NdJobWorker), reads its instruction pointer and
   resumes it; every 10 s prints, per thread, where it spent its time (eboot offsets, or module
   offsets outside the game). Finds the guest loop the job workers spin in. The filter can list
   several names separated by "+" or commas ("*" = every guest thread). Each sample also copies the top
   of the stack and counts the first game address on it (the likely caller), which tells what a
   thread is waiting in when its RIP is inside a host function. Nothing is allocated or printed
   while a thread is suspended. */
static uint64_t rip_image_size;
static void rip_where(uint64_t rip, char *out, size_t size);
enum { RIP_THREADS = 48, RIP_SLOTS = 4096, RIP_STACK_QWORDS = 1024 };
static uint64_t rip_keys[RIP_THREADS][RIP_SLOTS], caller_keys[RIP_THREADS][RIP_SLOTS];
static uint32_t rip_counts[RIP_THREADS][RIP_SLOTS], caller_counts[RIP_THREADS][RIP_SLOTS];
/* uc3: the first three game return addresses on the stack (a call instruction right before
   them), packed as 21-bit eboot offsets / 16: the guest call chain a host wait came from. */
static uint64_t chain_keys[RIP_THREADS][RIP_SLOTS];
/* uc3: the GPU command processor thread (filter "GpuCommandProcessor") is a host thread: its
   chain holds ucndc.exe return addresses instead. */
static int rip_host[RIP_THREADS];
static uint32_t chain_counts[RIP_THREADS][RIP_SLOTS];
static int rip_after_call(uint64_t v) {
    /* The image has unmapped holes: read the bytes before v safely. */
    uint8_t bytes[8];
    SIZE_T read = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)(v - 8), bytes, sizeof(bytes), &read) ||
        read != sizeof(bytes))
        return 0;
    const uint8_t *p = bytes + 8;
    if (p[-5] == 0xE8) return 1;                                   /* call rel32 */
    if (p[-2] == 0xFF && (p[-2 + 1] & 0xF8) == 0xD0) return 1;     /* call reg */
    if (p[-3] == 0xFF && (p[-2] & 0xF8) == 0x50) return 1;         /* call [reg+disp8] */
    if (p[-6] == 0xFF && (p[-5] == 0x15 || (p[-5] & 0xF8) == 0x90)) return 1; /* call [rip/reg+disp32] */
    if (p[-3] == 0x41 && p[-2] == 0xFF && (p[-1] & 0xF8) == 0xD0) return 1;  /* call r8-r15 */
    return 0;
}
static int rip_count(uint64_t *keys, uint32_t *counts, uint64_t key) {
    uint32_t slot = (uint32_t)((key * UINT64_C(0x9E3779B97F4A7C15)) >> 52) & (RIP_SLOTS - 1);
    for (int probe_n = 0; probe_n < 64; ++probe_n, slot = (slot + 1) & (RIP_SLOTS - 1)) {
        if (keys[slot] == key) { ++counts[slot]; return 1; }
        if (!keys[slot]) { keys[slot] = key; counts[slot] = 1; return 1; }
    }
    return 0;
}
static int rip_filter_match(const char *name, const char *filter) {
    if (!strcmp(filter, "*")) return 1;
    char token[64];
    for (const char *at = filter; *at;) {
        size_t n = strcspn(at, ",+");
        if (n && n < sizeof(token)) { memcpy(token, at, n); token[n] = 0; if (strstr(name, token)) return 1; }
        at += n; if (*at) ++at;
    }
    return 0;
}
static void rip_print_top(const char *label, uint64_t *keys, uint32_t *counts, uint32_t total, int top) {
    for (int k = 0; k < top; ++k) {
        uint32_t best = 0; int at = -1;
        for (int s = 0; s < RIP_SLOTS; ++s) if (counts[s] > best) { best = counts[s]; at = s; }
        if (at < 0) break;
        char where[MAX_PATH + 32];
        rip_where(keys[at], where, sizeof(where));
        printf("  %s %5.1f%%  %s\n", label, total ? 100.0 * best / total : 0.0, where);
        counts[at] = 0;
    }
}
static void rip_where(uint64_t rip, char *out, size_t size) {
    if (rip >= (uint64_t)(uintptr_t)image && rip - (uint64_t)(uintptr_t)image < rip_image_size) {
        snprintf(out, size, "eboot+0x%" PRIx64, rip - (uint64_t)(uintptr_t)image);
        return;
    }
    HMODULE module = NULL;
    char path[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)rip, &module) && module) {
        GetModuleFileNameA(module, path, sizeof(path));
        const char *base = strrchr(path, '\\');
        snprintf(out, size, "%s+0x%" PRIx64, base ? base + 1 : path, rip - (uint64_t)(uintptr_t)module);
    } else {
        snprintf(out, size, "0x%" PRIx64, rip);
    }
}
static void *rip_sampler(void *unused) {
    (void)unused;
    const char *filter = getenv("GOW3_RIP_SAMPLE_FILTER");
    if (!filter || !filter[0]) filter = "NdJobWorker";
    uint32_t ids[64];
    char names_now[64][32];
    for (;;) {
        HANDLE handles[RIP_THREADS];
        char tnames[RIP_THREADS][32];
        uint32_t samples[RIP_THREADS] = {0}, lost[RIP_THREADS] = {0};
        int count = 0;
        int listed = runtime_thread_snapshot(ids, names_now, 64);
        for (int i = 0; i < listed && count < RIP_THREADS; ++i) {
            if (!rip_filter_match(names_now[i], filter)) continue;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, ids[i]);
            if (!h) continue;
            handles[count] = h;
            memcpy(tnames[count], names_now[i], 32);
            memset(rip_keys[count], 0, sizeof(rip_keys[count]));
            memset(rip_counts[count], 0, sizeof(rip_counts[count]));
            memset(caller_keys[count], 0, sizeof(caller_keys[count]));
            memset(caller_counts[count], 0, sizeof(caller_counts[count]));
            memset(chain_keys[count], 0, sizeof(chain_keys[count]));
            memset(chain_counts[count], 0, sizeof(chain_counts[count]));
            rip_host[count] = 0;
            ++count;
        }
        unsigned long cp_id = gow3gpu_cp_thread_id();
        if (cp_id && count < RIP_THREADS && rip_filter_match("GpuCommandProcessor", filter)) {
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, cp_id);
            if (h) {
                handles[count] = h;
                snprintf(tnames[count], sizeof(tnames[count]), "GpuCommandProcessor");
                memset(rip_keys[count], 0, sizeof(rip_keys[count]));
                memset(rip_counts[count], 0, sizeof(rip_counts[count]));
                memset(caller_keys[count], 0, sizeof(caller_keys[count]));
                memset(caller_counts[count], 0, sizeof(caller_counts[count]));
                memset(chain_keys[count], 0, sizeof(chain_keys[count]));
                memset(chain_counts[count], 0, sizeof(chain_counts[count]));
                rip_host[count] = 1;
                ++count;
            }
        }
        if (!count) { Sleep(2000); continue; }
        ULONGLONG until = GetTickCount64() + 10000;
        while (GetTickCount64() < until) {
            for (int t = 0; t < count; ++t) {
                CONTEXT context;
                memset(&context, 0, sizeof(context));
                context.ContextFlags = CONTEXT_CONTROL;
                if (SuspendThread(handles[t]) == (DWORD)-1) continue;
                BOOL ok = GetThreadContext(handles[t], &context);
                static uint64_t stack[RIP_STACK_QWORDS];
                SIZE_T got = 0;
                BOOL stack_ok = FALSE;
                for (size_t want = RIP_STACK_QWORDS; ok && !stack_ok && want >= 8; want /= 4)
                    stack_ok = ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)context.Rsp,
                                                 stack, want * sizeof(stack[0]), &got);
                ResumeThread(handles[t]);
                if (!ok) continue;
                uint64_t rip = rip_host[t] ? context.Rip & ~UINT64_C(0x3F) : context.Rip;
                if (!rip_count(rip_keys[t], rip_counts[t], rip)) { ++lost[t]; continue; }
                ++samples[t];
                uint64_t caller = 0;
                for (size_t q = 0; stack_ok && q < got / sizeof(stack[0]); ++q) {
                    uint64_t v = stack[q];
                    if (v != rip && v > (uint64_t)(uintptr_t)image && v - (uint64_t)(uintptr_t)image < rip_image_size) {
                        caller = v;
                        break;
                    }
                }
                if (caller) rip_count(caller_keys[t], caller_counts[t], caller);
                uint64_t chain = 0;
                int links = 0;
                uint64_t chain_base = (uint64_t)(uintptr_t)image, chain_size = rip_image_size;
                if (rip_host[t]) {
                    static uint64_t exe_base, exe_size;
                    if (!exe_base) {
                        const unsigned char *base = (const unsigned char *)GetModuleHandleA(NULL);
                        const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
                        exe_size = nt->OptionalHeader.SizeOfImage;
                        exe_base = (uint64_t)(uintptr_t)base;
                    }
                    chain_base = exe_base;
                    chain_size = exe_size < (UINT64_C(1) << 25) ? exe_size : (UINT64_C(1) << 25);
                }
                for (size_t q = 0; stack_ok && q < got / sizeof(stack[0]) && links < 3; ++q) {
                    uint64_t v = stack[q];
                    if (v > chain_base + 8 && v - chain_base < chain_size && rip_after_call(v)) {
                        chain |= (((v - chain_base) >> 4) & 0x1FFFFF) << (21 * links);
                        ++links;
                    }
                }
                if (links) rip_count(chain_keys[t], chain_counts[t], chain);
            }
            Sleep(1);
        }
        for (int t = 0; t < count; ++t) {
            CloseHandle(handles[t]);
            printf("RIP samples (10 s) %s: %u samples", tnames[t], samples[t]);
            if (lost[t]) printf(", %u unbinned", lost[t]);
            printf("\n");
            rip_print_top(rip_host[t] ? "at64B " : "at    ", rip_keys[t], rip_counts[t], samples[t], rip_host[t] ? 150 : 10);
            rip_print_top("caller", caller_keys[t], caller_counts[t], samples[t], 5);
            for (int k = 0; k < 6; ++k) {
                uint32_t best = 0; int at = -1;
                for (int s2 = 0; s2 < RIP_SLOTS; ++s2) if (chain_counts[t][s2] > best) { best = chain_counts[t][s2]; at = s2; }
                if (at < 0) break;
                uint64_t c = chain_keys[t][at];
                printf("  chain  %5.1f%%  ~%s+0x%" PRIx64 " <- ~0x%" PRIx64 " <- ~0x%" PRIx64 " (16-byte units)\n",
                       samples[t] ? 100.0 * best / samples[t] : 0.0, rip_host[t] ? "ucndc" : "eboot",
                       (c & 0x1FFFFF) << 4, ((c >> 21) & 0x1FFFFF) << 4, ((c >> 42) & 0x1FFFFF) << 4);
                chain_counts[t][at] = 0;
            }
        }
        fflush(stdout);
    }
    return NULL;
}
#endif

/* ucndc: every GOW3_<NAME> option can also be given as UC_<NAME> (the new name wins when both
   are set); the code keeps reading GOW3_*. Priority 101 runs before the GPU library's C++ static
   initializers, which read options too. */
__attribute__((constructor(101))) static void uc_environment_aliases(void) {
#ifdef _WIN32
    char **env = _environ;
#else
    extern char **environ;
    char **env = environ;
#endif
    char pairs[64][512];
    int count = 0;
    for (; env && *env && count < 64; env++)
        if (strncmp(*env, "UC_", 3) == 0 && strchr(*env, '=') && strlen(*env) < sizeof(pairs[0]))
            strcpy(pairs[count++], *env);
    for (int i = 0; i < count; i++) {
        char *value = strchr(pairs[i], '=');
        *value++ = 0;
        char name[512];
        snprintf(name, sizeof(name), "GOW3_%s", pairs[i] + 3);
#ifdef _WIN32
        _putenv_s(name, value);
        SetEnvironmentVariableA(name, value[0] ? value : NULL);
#else
        setenv(name, value, 1);
#endif
    }
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
#ifndef _WIN32
    /* Keep host heap objects handed to the guest (thread handles, TLS) in the
       non-PIE brk heap, i.e. below 1 TiB: the guest packs pointers into 40 bits. */
    mallopt(M_ARENA_MAX,1);
    mallopt(M_MMAP_THRESHOLD,32*1024*1024);
#endif
    if (argc == 2 && !strcmp(argv[1], "--vulkan-only")) return vulkan_smoke();
#ifdef _WIN32
    /* The PS4 user range must stay free of host allocations (GPU driver, heaps), and the guest
     * TCB needs one of the 64 TEB TLS slots before DLLs (Vulkan drivers) take them. */
    runtime_memory_reserve();
    runtime_thread_tcb_offset();
#endif
    int cpu_only = 0, strict_imports = 0, check_imports = 0, game_hooks = 0;
    unsigned timeout_seconds = 10;
    const char *content_profile=NULL, *app0=NULL, *user_dir=NULL, *patch_file=NULL;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--cpu-only")) cpu_only = 1;
        else if (!strcmp(argv[i], "--strict-imports")) strict_imports = 1;
        else if (!strcmp(argv[i], "--check-imports")) check_imports = 1;
        else if (!strcmp(argv[i], "--content-profile") && i+1<argc) content_profile=argv[++i];
        else if (!strcmp(argv[i], "--app0") && i+1<argc) app0=argv[++i];
        else if (!strcmp(argv[i], "--user") && i+1<argc) user_dir=argv[++i];
        else if (!strcmp(argv[i], "--patches") && i+1<argc) patch_file=argv[++i];
        else if (!strcmp(argv[i], "--game-hooks")) game_hooks = 1;
        else if (!strcmp(argv[i], "--timeout") && i+1<argc) timeout_seconds=(unsigned)strtoul(argv[++i],NULL,10);
        else { fprintf(stderr, "Unknown option: %s\n", argv[i]); return 1; }
    }
    if (argc < 2) {
        fprintf(stderr, "Usage: %s boot.bin [--cpu-only] [--strict-imports] [--check-imports] [--content-profile file] [--app0 dir] [--user dir] [--patches file] [--timeout seconds] | --vulkan-only\n", argv[0]);
        return 1;
    }
    if (content_profile) {
        FILE *profile=fopen(content_profile,"rb");
        unsigned char data[28];
        if (!profile) fail("cannot open content profile");
        if (fread(data,1,sizeof(data),profile)!=sizeof(data) || fgetc(profile)!=EOF || memcmp(data,"G3CONT01",8)) fail("invalid content profile");
        fclose(profile);
        uint32_t values[5];
        for (unsigned i=0;i<5;++i) {
            unsigned char *p=data+8+i*4;
            values[i]=(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
        }
        runtime_content_configure(values);
    }
    char title_id[16]="";
    if (app0) {
        runtime_file_configure(app0, user_dir ? user_dir : "user");
        char sfo[4096], id[16]="";
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0);
        sfo_value(sfo,"TITLE_ID",title_id,sizeof(title_id),NULL);
        if (sfo_value(sfo,"INSTALL_DIR_SAVEDATA",id,sizeof(id),NULL) || sfo_value(sfo,"TITLE_ID",id,sizeof(id),NULL))
            runtime_savedata_configure(id);
    }
    gow3gpu_register_kernel();
#ifdef _WIN32
    SYSTEM_INFO system_info; GetSystemInfo(&system_info); page_size = system_info.dwPageSize;
    AddVectoredExceptionHandler(1, vectored_fault);
    SetUnhandledExceptionFilter(unhandled_fault);
    if (getenv("GOW3_STRICT_HANDLES")) {
        PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY strict={0};
        strict.RaiseExceptionOnInvalidHandleReference=1;
        strict.HandleExceptionsPermanentlyEnabled=1;
        if (!SetProcessMitigationPolicy(ProcessStrictHandleCheckPolicy,&strict,sizeof(strict)))
            fprintf(stderr,"GOW3_STRICT_HANDLES: policy not set (%lu)\n",GetLastError());
    }
    (void)timeout_seconds; /* no watchdog on Windows */
#else
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    struct sigaction sa = {0}; sa.sa_sigaction = fault; sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    Dl_info self_info;
    if (dladdr((void *)main,&self_info)) exe_base=(uintptr_t)self_info.dli_fbase;
    struct sigaction dump = {0}; dump.sa_sigaction = thread_dump; dump.sa_flags = SA_SIGINFO|SA_RESTART;
    sigemptyset(&dump.sa_mask); sigaction(SIGUSR2, &dump, NULL);
    struct sigaction alarm_action = {0}; alarm_action.sa_sigaction = watchdog; alarm_action.sa_flags = SA_SIGINFO;
    sigemptyset(&alarm_action.sa_mask); sigaction(SIGALRM, &alarm_action, NULL);
    alarm(timeout_seconds); /* 0 disables the watchdog */
#endif
    FILE *f = fopen(argv[1], "rb");
    if (!f) fail("cannot open boot file; run prepare.py first");
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || (memcmp(magic, "G3PROBE1", 8) && memcmp(magic, "G3PROBE2", 8) && memcmp(magic,"G3PROBE3",8) && memcmp(magic,"G3PROBE4",8) && memcmp(magic,"G3PROBE5",8))) fail("bad boot file signature");
    uint64_t size = read64(f), entry = read64(f), ns = read64(f), nr = read64(f);
    import_count = read64(f);
    uint64_t capabilities = memcmp(magic, "G3PROBE1", 8) ? read64(f) : 0;
    if (capabilities & ~UINT64_C(1)) fail("unknown runtime capabilities");
    runtime_start(strict_imports ? 0 : capabilities);
    if (!size || size > 512*1024*1024 || entry >= size || !ns || ns > 64 || nr > 1000000 || import_count > 100000)
        fail("boot file limits exceeded");
    int multi=!memcmp(magic,"G3PROBE5",8);
    int linked=!memcmp(magic,"G3PROBE3",8) || !memcmp(magic,"G3PROBE4",8) || multi;
    int native_libc=linked && !strict_imports && (capabilities&1);
    uint64_t main_tls[4]={0};
    uint64_t nb=0,procparam=0;
    uint64_t *bindings=calloc(import_count ? import_count : 1,sizeof(*bindings));
    uint64_t *binding_kinds=calloc(import_count ? import_count : 1,sizeof(*binding_kinds));
    if (!bindings || !binding_kinds) fail("allocation failed");
    if (multi) {
        /* G3PROBE5: procparam, eboot TLS, module table, bindings (link_modules.py). */
        procparam=read64(f);
        for (int i=0;i<4;++i) main_tls[i]=read64(f);
        module_count=read64(f);
        if (!module_count || module_count>sizeof(modules)/sizeof(*modules)) fail("invalid module count");
        for (uint64_t m=0;m<module_count;++m) {
            LinkedModule *x=&modules[m];
            x->base=read64(f); x->size=read64(f); x->init=read64(f); x->tls_address=read64(f);
            x->tls_memsz=read64(f); x->tls_filesz=read64(f); x->tls_module=read64(f);
        }
        nb=read64(f);
    } else if (linked) {
        LinkedModule *x=&modules[0];
        module_count=1;
        x->base=read64(f); x->size=read64(f); x->init=read64(f);
        x->tls_address=read64(f); x->tls_memsz=read64(f); x->tls_filesz=read64(f); x->tls_module=2; nb=read64(f);
        procparam=read64(f);
        if (!memcmp(magic,"G3PROBE4",8)) for (int i=0;i<4;++i) main_tls[i]=read64(f);
    }
    if (linked) {
        if (main_tls[1]>main_tls[2] || main_tls[2]>1024*1024 || main_tls[0]>size ||
            main_tls[1]>size-main_tls[0] || (main_tls[3] & (main_tls[3]-1)) || main_tls[3]>4096)
            fail("invalid eboot TLS metadata");
        for (uint64_t m=0;m<module_count;++m) {
            LinkedModule *x=&modules[m];
            if (x->base>=size || !x->size || x->size>size-x->base || x->init<x->base || x->init-x->base>=x->size ||
                (x->tls_module && (x->tls_address>=size || x->tls_memsz>size-x->tls_address || x->tls_memsz>1024*1024 ||
                                   x->tls_filesz>x->tls_memsz || x->tls_module<2 || x->tls_module>7)))
                fail("invalid linked module metadata");
        }
        if (nb>import_count) fail("invalid binding count");
        for (uint64_t i=0;i<nb;++i) {
            uint64_t index=read64(f),address=read64(f),kind=read64(f);
            int inside=0;
            for (uint64_t m=0;m<module_count;++m)
                if (address>=modules[m].base && address-modules[m].base<modules[m].size) inside=1;
            if (index>=import_count || !inside || (kind!=1 && kind!=2) || bindings[index]) fail("invalid native binding");
            bindings[index]=address; binding_kinds[index]=kind;
        }
    }
    Segment *segments = calloc(ns, sizeof(*segments));
    Reloc *relocs = calloc(nr ? nr : 1, sizeof(*relocs));
    names = calloc(import_count ? import_count : 1, sizeof(*names));
    if (!segments || !relocs || !names) fail("allocation failed");
    for (uint64_t i = 0; i < ns; ++i) {
        segments[i].address = read64(f);
        segments[i].size = read64(f);
        segments[i].flags = read64(f);
        if (segments[i].address > size || segments[i].size > size - segments[i].address ||
            segments[i].address % page_size || segments[i].flags > 7) fail("bad segment");
    }
    if (fread(names, 128, import_count, f) != import_count) fail("truncated import names");
    for (uint64_t i = 0; i < import_count; ++i)
        if (!memchr(names[i], 0, 128)) fail("unterminated import name");
    for (uint64_t i = 0; i < nr; ++i) {
        relocs[i].target = read64(f);
        relocs[i].kind = read64(f);
        relocs[i].value = read64(f);
        relocs[i].addend = read64(f);
        if (!mapped(segments, ns, relocs[i].target, 8) || relocs[i].kind > 2 ||
            (relocs[i].kind && relocs[i].value >= import_count) ||
            (relocs[i].kind != 2 && relocs[i].addend) || relocs[i].addend >= page_size) fail("bad relocation");
    }
    for (uint64_t m=0;m<module_count;++m)
        if (!mapped(segments,ns,modules[m].init,1) || (modules[m].tls_module && !mapped(segments,ns,modules[m].tls_address,modules[m].tls_memsz)))
            fail("unmapped module metadata");
    if (module_count && !mapped(segments,ns,procparam,64)) fail("unmapped procparam");
    for (uint64_t i=0;i<import_count;++i) {
        if (!bindings[i]) continue;
        if (!mapped(segments,ns,bindings[i],1)) fail("unmapped native export");
        if (binding_kinds[i]==1) {
            int executable=0;
            for (uint64_t s=0;s<ns;++s)
                if ((segments[s].flags&1) && bindings[i]>=segments[s].address &&
                    bindings[i]-segments[s].address<segments[s].size) executable=1;
            if (!executable) fail("native function is not executable");
        }
    }
    image = allocate(round_page(size));
    printf("Image base: %p (guest code addresses in traces are image offsets from here)\n",
           (void *)image);
    if (getenv("GOW3_LOW_IMAGE") && (uintptr_t)image+size>UINT64_C(0x80000000))
        printf("GOW3_LOW_IMAGE: no free host range below 2 GiB, image stays high\n");
    if (fread(image, 1, size, f) != size || fgetc(f) != EOF) fail("incorrect memory image size");
    fclose(f);
    if (!cpu_only) {
        char title[128]="PS4 game", serial[16]="UNKNOWN", sfo[4096];
        uint32_t attributes=0;
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0 ? app0 : ".");
        sfo_value(sfo,"TITLE",title,sizeof(title),NULL);
        sfo_value(sfo,"TITLE_ID",serial,sizeof(serial),NULL);
        sfo_value(sfo,"ATTRIBUTE",NULL,0,&attributes);
        uint64_t sdk=0;
        if (procparam) memcpy(&sdk,image+procparam+16,8); /* procparam: size, magic, count, sdk_version */
        Gow3GpuConfig gpu={title,serial,user_dir ? user_dir : "user",(uint32_t)sdk,attributes,1920,1080};
        gpu_enabled=1; /* page tracking starts while the rasterizer registers guest memory */
        if (gow3gpu_init(&gpu)) fail("GPU initialization failed");
        printf("GPU: window and Vulkan presenter ready; SDK 0x%08x, %u HLE symbols\n",(unsigned)sdk,gow3gpu_symbol_count());
    }
    unsigned char *traps = allocate(round_page((import_count + 1) * 32));
    unsigned char *data_traps = allocate((import_count + 1) * page_size);
    protect(data_traps, (import_count + 1) * page_size, 0);
    for (uint64_t i = 0; i < import_count; ++i) {
        unsigned char *t = traps + i * 32;
        /* SysV: mov edi, index; movabs rax, handler; jmp rax. No fake return values. */
        uint32_t index = (uint32_t)i;
        uintptr_t handler = (uintptr_t)unresolved;
        t[0] = 0x48; t[1] = 0x89; t[2] = 0xfe; /* mov rsi,rdi: preserve arg0 */
        t[3] = 0xbf; memcpy(t + 4, &index, 4);
        t[8] = 0x48; t[9] = 0xb8; memcpy(t + 10, &handler, 8);
        t[18] = 0xff; t[19] = 0xe0;
    }
    unsigned char *import_thunks = NULL;
    if (getenv("GOW3_IMPORT_COUNT") && getenv("GOW3_IMPORT_COUNT")[0] == '1' && !check_imports) {
        import_thunks = allocate(round_page((import_count + 1) * 32));
        import_calls = calloc((import_count + 1) * IMPORT_CALL_STRIDE, sizeof(*import_calls));
        if (!import_calls) fail("allocation failed");
    }
    for (uint64_t i = 0; i < nr; ++i) {
        uintptr_t value = relocs[i].kind == 2 ? (uintptr_t)(data_traps + page_size * relocs[i].value + relocs[i].addend)
                        : relocs[i].kind == 1 ? (uintptr_t)(traps + 32 * relocs[i].value)
                        : (uintptr_t)image + relocs[i].value;
        if (relocs[i].kind) {
            uintptr_t resolved = runtime_resolve(names[relocs[i].value], relocs[i].kind == 2);
            if (resolved) value = resolved + relocs[i].addend;
            else if (native_libc && bindings[relocs[i].value]) {
                uint64_t address=bindings[relocs[i].value];
                if (binding_kinds[relocs[i].value]!=relocs[i].kind || !mapped(segments,ns,address,relocs[i].addend+1)) fail("native export kind/range mismatch");
                value=(uintptr_t)image+address+relocs[i].addend;
            }
        }
        if (import_thunks && relocs[i].kind == 1 && relocs[i].addend == 0 &&
            value != (uintptr_t)(traps + 32 * relocs[i].value)) {
            unsigned char *t = import_thunks + 32 * relocs[i].value;
            if (!t[0]) {
                uintptr_t counter = (uintptr_t)&import_calls[relocs[i].value * IMPORT_CALL_STRIDE];
                t[0] = 0x49; t[1] = 0xbb; memcpy(t + 2, &counter, 8);          /* mov r11, counter */
                t[10] = 0xf0; t[11] = 0x49; t[12] = 0xff; t[13] = 0x03;        /* lock inc qword [r11] */
                t[14] = 0x49; t[15] = 0xbb; memcpy(t + 16, &value, 8);         /* mov r11, target */
                t[24] = 0x41; t[25] = 0xff; t[26] = 0xe3;                      /* jmp r11 */
            }
            value = (uintptr_t)t;
        }
        memcpy(image + relocs[i].target, &value, 8);
    }
    if (import_thunks) {
        protect(import_thunks, round_page((import_count + 1) * 32), 5);
        pthread_t reporter;
        if (!pthread_create(&reporter, NULL, import_count_report, NULL)) pthread_detach(reporter);
        printf("GOW3_IMPORT_COUNT: import calls counted, report every 10 s\n");
    }
#ifdef _WIN32
    if (getenv("GOW3_RIP_SAMPLE") && getenv("GOW3_RIP_SAMPLE")[0] == '1' && !check_imports) {
        rip_image_size = size;
        pthread_t sampler;
        if (!pthread_create(&sampler, NULL, rip_sampler, NULL)) pthread_detach(sampler);
        printf("GOW3_RIP_SAMPLE: sampling guest threads every ~1 ms, report every 10 s\n");
    }
#endif
    if (check_imports) {
        /* Lists every import the game would stop at, without running it (needs the GPU
         * library: without --cpu-only its symbols are registered above). */
        unsigned char *checked=calloc(import_count ? import_count : 1,1);
        if (!checked) fail("allocation failed");
        uint64_t missing=0;
        for (uint64_t i=0;i<nr;++i) {
            uint64_t index=relocs[i].value;
            if (!relocs[i].kind || checked[index]) continue;
            checked[index]=1;
            if (runtime_resolve(names[index],relocs[i].kind==2) || (native_libc && bindings[index])) continue;
            printf("MISSING: %s (%s)\n",names[index],runtime_import_name(names[index]));
            ++missing;
        }
        printf("Imports: %" PRIu64 " missing\n",missing);
        fflush(NULL);
        _exit(missing ? 1 : 0); /* no destructors: GPU threads are running */
    }
    if (patch_file) apply_patches(patch_file, segments, ns, relocs, nr);
    /* uc3: the God of War III hooks below are only searched for when run.py says the game is
     * God of War III (its patch profile, scripts/patches.py): in another game a chance
     * signature match would patch unrelated code. */
    if (gpu_enabled && game_hooks) {
        install_orb_multiplier(segments, ns, size);
        install_cheats(segments, ns, size);
        install_actor_watch(segments, ns, size);
        install_stat_multipliers(segments, ns, size);
    } else if (gpu_enabled) {
        printf("Game hooks: none for title %s (God of War III cheats and multipliers off)\n", title_id[0] ? title_id : "?");
    }
    if (gpu_enabled && getenv("GOW3_EOP_TICK_WAIT") && getenv("GOW3_EOP_TICK_WAIT")[0]=='1')
        patch_eop_tick_wait(size);
    if (gpu_enabled && getenv("GOW3_SCREAM_LOG") && getenv("GOW3_SCREAM_LOG")[0]=='1')
        install_scream_log(size);
    if (gpu_enabled && getenv("GOW3_DC_TRACE") && getenv("GOW3_DC_TRACE")[0]=='1')
        install_dc_trace(size);
#ifdef _WIN32
    if (gpu_enabled && getenv("GOW3_PLAYER_POS") && getenv("GOW3_PLAYER_POS")[0]=='1')
        install_player_pos(size);
    if (gpu_enabled && getenv("GOW3_DT_SCAN") && getenv("GOW3_DT_SCAN")[0]=='1')
        install_dt_scan(segments, ns);
#endif
    if ((getenv("GOW3_JOB_SPIN") && getenv("GOW3_JOB_SPIN")[0]) || (getenv("GOW3_JOB_SLEEP") && getenv("GOW3_JOB_SLEEP")[0] == '1'))
        patch_job_spin(size, getenv("GOW3_JOB_SPIN") && getenv("GOW3_JOB_SPIN")[0] ? getenv("GOW3_JOB_SPIN") : NULL,
                       getenv("GOW3_JOB_SLEEP") && getenv("GOW3_JOB_SLEEP")[0] == '1');
#ifdef _WIN32
    if (getenv("GOW3_CB_WATCH") && getenv("GOW3_CB_WATCH")[0] == '1' && !check_imports) cbw_install(size);
    if (getenv("GOW3_UC1_BIND_RETRY") && getenv("GOW3_UC1_BIND_RETRY")[0] == '1' && !check_imports) ubr_install(size);
    const int res_set = gpu_enabled && getenv("GOW3_UC3_RES") && getenv("GOW3_UC3_RES")[0];
    const int res_log = gpu_enabled && getenv("GOW3_UC3_RES_LOG") && getenv("GOW3_UC3_RES_LOG")[0] == '1';
    Uc3ResSite res_sites[UC_RES_MAX_SITES];
    const char *res_game = "UC3";
    size_t res_n = 0;
    if (res_set || res_log) res_n = uc_res_find(size, res_sites, &uc_res_struct, &res_game);
    if (res_set)
        patch_uc3_resolution(res_sites, res_n, res_game, getenv("GOW3_UC3_RES"),
                             getenv("GOW3_UC3_RES_ALL") && getenv("GOW3_UC3_RES_ALL")[0] == '1');
    if (gpu_enabled)
        patch_uc3_rt_heap(size);
    if (res_log && res_n && mapped(segments, ns, uc_res_struct, 0x88)) {
        pthread_t res_log_thread;
        if (!pthread_create(&res_log_thread, NULL, uc3_res_log, NULL)) pthread_detach(res_log_thread);
        printf("%s resolution: logging the video structure at %#" PRIx64 "\n", res_game, uc_res_struct);
    }
#endif
    /* ucndc: mouse camera (input.ini with mouse_look=1; GOW3_MOUSE_CAMERA=0 keeps the mouse a stick) */
    if (gpu_enabled && runtime_pad_mouse_camera()) install_game_callbacks(segments, ns, size);
    if (gpu_enabled && runtime_pad_mouse_camera()) install_call98_log(segments, ns, size);
#ifdef _WIN32
    printf("Guest TCB loads redirected to the TEB TLS slot: %" PRIu64 "\n", patch_tcb_loads(segments, ns));
#endif
    protect(traps, round_page((import_count + 1) * 32), 5);
    protect(image, round_page(size), 0);
    int executable_entry = 0;
    for (uint64_t i = 0; i < ns; ++i) {
        protect(image + segments[i].address, round_page(segments[i].size), (unsigned)segments[i].flags);
        if ((segments[i].flags & 1) && entry >= segments[i].address && entry - segments[i].address < segments[i].size)
            executable_entry = 1;
    }
    if (!executable_entry) fail("entry is not executable");
    printf("Mapped %" PRIu64 " bytes, %" PRIu64 " segments; applied %" PRIu64 " relocations\n", size, ns, nr);
    if (native_libc) {
        for (uint64_t m=0;m<module_count;++m) {
            int init_executable=0;
            for (uint64_t i=0;i<ns;++i)
                if ((segments[i].flags&1) && modules[m].init>=segments[i].address && modules[m].init-segments[i].address<segments[i].size) init_executable=1;
            if (!init_executable) fail("module init is not executable");
            if (modules[m].tls_module)
                runtime_set_module_tls(modules[m].tls_module,image+modules[m].tls_address,modules[m].tls_filesz,modules[m].tls_memsz);
        }
        runtime_set_main_tls(image+main_tls[0],main_tls[1],main_tls[2],main_tls[3]);
        runtime_thread_attach_main();
        runtime_set_procparam(image+procparam);
        /* Dependencies start in link order (libc first), as the PS4 dynamic linker does. */
        for (uint64_t m=0;m<module_count;++m) {
            printf("Starting linked module %" PRIu64 " at image offset 0x%" PRIx64 "; native bindings=%" PRIu64 "\n",m,modules[m].init,nb);
            typedef int (ABI *ModuleInit)(uint64_t,void *,void *);
            int result=((ModuleInit)(image+modules[m].init))(0,NULL,NULL);
            printf("Module %" PRIu64 " initializer returned %d\n",m,result);
            if (result) fail("module initializer failed");
        }
    }
    printf("Entering original x86-64 code at guest offset 0x%" PRIx64 "\n", entry);
    entered_game=1;
    static char program[256];
    snprintf(program, sizeof(program), "/app0/%s", runtime_executable_name());
    struct { uint64_t argc; const char *argv[18]; } params = {1, {program, NULL}};
    params.argc += (uint64_t)runtime_guest_args(params.argv + 1, 16);
    params.argv[params.argc] = NULL;
    if (params.argc > 1) {
        printf("Guest arguments:");
        for (uint64_t i = 1; i < params.argc; ++i) printf(" %s", params.argv[i]);
        printf("\n");
    }
#ifdef _WIN32
    typedef void (ABI *Entry)(void *, void (ABI *)(void));
    ((Entry)(image + entry))(&params, guest_exit);
#else
    /* The guest main thread runs on a stack below 1 TiB like PS4 stacks. */
    enum { MAIN_STACK=8*1024*1024 };
    unsigned char *stack=runtime_low_map(MAIN_STACK,PROT_READ|PROT_WRITE);
    if (!stack) fail("cannot allocate guest main stack");
    enter_on_stack(image+entry,&params,(void *)guest_exit,stack+MAIN_STACK-64);
#endif
    fail("entry unexpectedly returned");
}
