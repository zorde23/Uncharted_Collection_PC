/* uc3: libSceFiber for Uncharted 3, whose job system (NdJobWorkerThread, "NdJob Fiber") runs
 * every job on a fiber and moves suspended fibers between worker threads.
 *
 * A fiber lives in the guest's 256-byte SceFiber and runs on the stack the guest gives it.
 * uc3_fiber_swap saves the System V callee-saved registers, MXCSR and the x87 control word of
 * one context and loads another's; the guest TCB stays per thread (the TEB TLS slot), as on
 * the PS4, so a fiber that moves to another thread sees that thread's TLS.
 *
 * Three rules keep this sound:
 *   - a fiber that switches away is marked suspended only once its registers are saved (the
 *     code that lands in the next context does it), so another thread cannot resume it early;
 *   - per-thread state is read again after every switch through thread_state(), which the
 *     compiler cannot fold across a switch (the fiber may now run on another thread);
 *   - on Windows the TEB stack limits follow the running stack, as with native fibers. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define FIBER_ERROR_NULL ((int32_t)0x80590001)
#define FIBER_ERROR_ALIGNMENT ((int32_t)0x80590002)
#define FIBER_ERROR_RANGE ((int32_t)0x80590003)
#define FIBER_ERROR_INVALID ((int32_t)0x80590004)
#define FIBER_ERROR_PERMISSION ((int32_t)0x80590005)
#define FIBER_ERROR_STATE ((int32_t)0x80590006)
#define FIBER_CONTEXT_MINIMUM 512
#define FIBER_MAGIC 0x46334355u /* "UC3F" */

/* Offsets are used by uc3_fiber_swap below. */
typedef struct {
    uint64_t rbx, rbp, r12, r13, r14, r15, rsp, rip; /* 0..56 */
    uint32_t mxcsr; uint16_t fcw, pad;               /* 64, 68 */
} FiberRegs;

enum { FIBER_IDLE, FIBER_RUNNING, FIBER_SWITCHING, FIBER_SUSPENDED };
typedef struct {
    uint32_t magic, state;
    uint64_t entry, arg_init, arg_run;
    unsigned char *stack_low; uint64_t stack_size;
    uint64_t *arg_out;  /* where a resumer writes argOnRun (the suspended call's out pointer) */
    FiberRegs regs;
    char name[32];
} Fiber;
_Static_assert(sizeof(Fiber) <= 256, "SceFiber is 256 bytes");

ABI void uc3_fiber_swap(FiberRegs *save, const FiberRegs *load);
void uc3_fiber_start(void);
__asm__(".text\n.globl uc3_fiber_swap\nuc3_fiber_swap:\n"
        " movq %rbx,0(%rdi)\n movq %rbp,8(%rdi)\n movq %r12,16(%rdi)\n movq %r13,24(%rdi)\n"
        " movq %r14,32(%rdi)\n movq %r15,40(%rdi)\n leaq 8(%rsp),%rax\n movq %rax,48(%rdi)\n"
        " movq (%rsp),%rax\n movq %rax,56(%rdi)\n stmxcsr 64(%rdi)\n fnstcw 68(%rdi)\n"
        " movq 0(%rsi),%rbx\n movq 8(%rsi),%rbp\n movq 16(%rsi),%r12\n movq 24(%rsi),%r13\n"
        " movq 32(%rsi),%r14\n movq 40(%rsi),%r15\n ldmxcsr 64(%rsi)\n fldcw 68(%rsi)\n"
        " movq 48(%rsi),%rsp\n jmpq *56(%rsi)\n"
        /* A new fiber starts here with its SceFiber in r12 and a 16-byte aligned stack. */
        ".globl uc3_fiber_start\nuc3_fiber_start:\n movq %r12,%rdi\n call uc3_fiber_main\n ud2\n");

typedef struct {
    FiberRegs regs;        /* the thread's own context while it runs fibers */
    Fiber *current;        /* running fiber, NULL on the thread's own context */
    Fiber *previous;       /* switched away from, marked suspended once landed */
    uint64_t *run_return;  /* sceFiberRun's argOnReturn */
    void *stack_base, *stack_limit;
} FiberThread;
static _Thread_local FiberThread fiber_thread;
/* Not foldable across a switch: the volatile asm hides which thread's block this is. */
static __attribute__((noinline)) FiberThread *thread_state(void) {
    FiberThread *t=&fiber_thread;
    __asm__ volatile("" : "+r"(t));
    return t;
}

static uint64_t created, switches;

static void set_stack_limits(void *base, void *limit) {
#ifdef _WIN32
    NT_TIB *tib=(NT_TIB *)NtCurrentTeb();
    tib->StackBase=base; tib->StackLimit=limit;
#else
    (void)base; (void)limit;
#endif
}
static void enter_fiber_stack(Fiber *f) { set_stack_limits(f->stack_low+f->stack_size,f->stack_low); }

/* Runs in every context right after a switch lands: the context left behind is now saved. */
static void complete_switch(void) {
    FiberThread *t=thread_state();
    Fiber *previous=t->previous;
    t->previous=NULL;
    if (previous) __atomic_store_n(&previous->state,FIBER_SUSPENDED,__ATOMIC_RELEASE);
}

ABI __attribute__((noreturn, used)) void uc3_fiber_main(Fiber *f) {
    complete_switch();
    ((void (ABI *)(uint64_t,uint64_t))f->entry)(f->arg_init,f->arg_run);
    fprintf(stderr,"STOP: fiber \"%s\" returned from its entry function (it must return to the thread)\n",f->name);
    runtime_report();
    exit(23);
}

/* Takes a fiber that is idle or suspended for running; a fiber still saving its registers is
 * waited for. Delivers argOnRun to it (its entry argument or its suspended call's out pointer). */
static int32_t claim(Fiber *f, uint64_t arg) {
    if (!f) return FIBER_ERROR_NULL;
    if ((uintptr_t)f & 7) return FIBER_ERROR_ALIGNMENT;
    if (f->magic!=FIBER_MAGIC) return FIBER_ERROR_INVALID;
    for (;;) {
        uint32_t state=__atomic_load_n(&f->state,__ATOMIC_ACQUIRE);
        if (state==FIBER_SWITCHING) { __builtin_ia32_pause(); continue; }
        if (state!=FIBER_IDLE && state!=FIBER_SUSPENDED) return FIBER_ERROR_STATE;
        if (__atomic_compare_exchange_n(&f->state,&state,FIBER_RUNNING,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) {
            if (state==FIBER_IDLE) f->arg_run=arg;
            else if (f->arg_out) *f->arg_out=arg;
            return 0;
        }
    }
}

static ABI int32_t fiber_initialize(Fiber *f, const char *name, uint64_t entry, uint64_t arg_init,
                                    void *context, uint64_t size, const void *opt, uint32_t flags, uint32_t build) {
    (void)opt; (void)flags; (void)build;
    if (!f || !entry) return FIBER_ERROR_NULL;
    if ((uintptr_t)f & 7) return FIBER_ERROR_ALIGNMENT;
    if (!context) {
        /* A fiber without its own stack runs on the thread's: not used by Uncharted 3 so far. */
        fprintf(stderr,"STOP: sceFiberInitialize(\"%s\") without a context stack is not supported\n",name ? name : "");
        runtime_report();
        exit(21);
    }
    if ((uintptr_t)context & 15 || size & 15) return FIBER_ERROR_ALIGNMENT;
    if (size<FIBER_CONTEXT_MINIMUM) return FIBER_ERROR_RANGE;
    memset(f,0,sizeof(*f));
    f->magic=FIBER_MAGIC; f->state=FIBER_IDLE;
    f->entry=entry; f->arg_init=arg_init;
    f->stack_low=context; f->stack_size=size;
    snprintf(f->name,sizeof(f->name),"%s",name ? name : "");
    f->regs.r12=(uint64_t)(uintptr_t)f;
    f->regs.rsp=((uint64_t)(uintptr_t)(f->stack_low+size))&~UINT64_C(15);
    f->regs.rip=(uint64_t)(uintptr_t)uc3_fiber_start;
    f->regs.mxcsr=0x1F80; f->regs.fcw=0x037F; /* FreeBSD defaults: all exceptions masked */
    if (runtime_guest_mxcsr()) f->regs.mxcsr=runtime_guest_mxcsr(); /* GOW3_GUEST_MXCSR: the PS4's 0x9fc0 */
    if (!__atomic_fetch_add(&created,1,__ATOMIC_RELAXED))
        printf("Runtime: first fiber \"%s\" (%llu KiB stack)\n",f->name,(unsigned long long)(size/1024));
    return 0;
}
static ABI int32_t fiber_opt_initialize(void *opt) {
    if (!opt) return FIBER_ERROR_NULL;
    if ((uintptr_t)opt & 7) return FIBER_ERROR_ALIGNMENT;
    memset(opt,0,128); return 0;
}
static ABI int32_t fiber_finalize(Fiber *f) {
    if (!f) return FIBER_ERROR_NULL;
    if ((uintptr_t)f & 7) return FIBER_ERROR_ALIGNMENT;
    if (f->magic!=FIBER_MAGIC) return FIBER_ERROR_INVALID;
    uint32_t state=__atomic_load_n(&f->state,__ATOMIC_ACQUIRE);
    if (state!=FIBER_IDLE && state!=FIBER_SUSPENDED) return FIBER_ERROR_STATE;
    f->magic=0; return 0;
}
/* From the thread's own context: runs the fiber until a fiber returns to this thread. */
static ABI int32_t fiber_run(Fiber *f, uint64_t arg, uint64_t *arg_on_return) {
    FiberThread *t=thread_state();
    if (t->current) return FIBER_ERROR_PERMISSION;
    int32_t r=claim(f,arg);
    if (r) return r;
#ifdef _WIN32
    NT_TIB *tib=(NT_TIB *)NtCurrentTeb();
    t->stack_base=tib->StackBase; t->stack_limit=tib->StackLimit;
#endif
    t->current=f; t->run_return=arg_on_return;
    __atomic_fetch_add(&switches,1,__ATOMIC_RELAXED);
    enter_fiber_stack(f);
    uc3_fiber_swap(&t->regs,&f->regs);
    complete_switch();
    return 0;
}
/* From a fiber: suspends it and runs another; returns when something resumes this fiber. */
static ABI int32_t fiber_switch(Fiber *to, uint64_t arg, uint64_t *arg_on_run) {
    FiberThread *t=thread_state();
    Fiber *self=t->current;
    if (!self) return FIBER_ERROR_PERMISSION;
    if (to==self) return FIBER_ERROR_STATE;
    int32_t r=claim(to,arg);
    if (r) return r;
    self->arg_out=arg_on_run;
    __atomic_store_n(&self->state,FIBER_SWITCHING,__ATOMIC_RELAXED);
    t->previous=self; t->current=to;
    __atomic_fetch_add(&switches,1,__ATOMIC_RELAXED);
    enter_fiber_stack(to);
    uc3_fiber_swap(&self->regs,&to->regs);
    complete_switch(); /* possibly on another thread now */
    return 0;
}
static ABI int32_t fiber_return_to_thread(uint64_t arg_on_return, uint64_t *arg_on_run) {
    FiberThread *t=thread_state();
    Fiber *self=t->current;
    if (!self) return FIBER_ERROR_PERMISSION;
    self->arg_out=arg_on_run;
    __atomic_store_n(&self->state,FIBER_SWITCHING,__ATOMIC_RELAXED);
    if (t->run_return) *t->run_return=arg_on_return;
    t->previous=self; t->current=NULL;
    set_stack_limits(t->stack_base,t->stack_limit);
    uc3_fiber_swap(&self->regs,&t->regs);
    complete_switch(); /* resumed by sceFiberRun or sceFiberSwitch, maybe on another thread */
    return 0;
}
static ABI int32_t fiber_get_self(Fiber **out) {
    if (!out) return FIBER_ERROR_NULL;
    Fiber *self=thread_state()->current;
    if (!self) return FIBER_ERROR_PERMISSION;
    *out=self; return 0;
}

static const RuntimeExport exports[]={
    {"_sceFiberInitializeImpl",fiber_initialize}, {"sceFiberOptParamInitialize",fiber_opt_initialize},
    {"sceFiberFinalize",fiber_finalize}, {"sceFiberRun",fiber_run}, {"sceFiberSwitch",fiber_switch},
    {"sceFiberReturnToThread",fiber_return_to_thread}, {"sceFiberGetSelf",fiber_get_self},
};
uintptr_t runtime_fiber_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_fiber_report(void) {
    printf("Runtime: fibers created=%llu, switches=%llu\n",(unsigned long long)created,(unsigned long long)switches);
}
