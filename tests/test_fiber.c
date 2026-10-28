/* uc3: libSceFiber contracts (src/runtime_fiber.c): run/return, switch, arguments both ways,
 * migration between threads, a multi-thread job-system stress run, and error codes.
 * Windows: ninja -C out/gpu fiber-test && out/gpu/fiber-test.exe
 * Linux:   gcc -std=gnu11 -O2 -Wall -Wextra -Werror -Isrc tests/test_fiber.c -lpthread -o out/fiber-test */
#include "../src/runtime_fiber.c"
#include <pthread.h>
#include <sched.h>

/* runtime.c and runtime_thread.c pieces runtime_fiber.c uses. */
void runtime_report(void) {}
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) { (void)table; (void)count; (void)nid; return 0; }
unsigned runtime_guest_mxcsr(void) { return 0; }

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

#define STACK (64 * 1024)
#define SLOTS 140 /* 2 * JOBS for the stress run, then the error tests */
static _Alignas(16) unsigned char stacks[SLOTS][STACK];
static _Alignas(8) unsigned char fibers[SLOTS][256];
#define FIBER(i) ((Fiber *)fibers[i])

static int stack_inside(void) {
#ifdef _WIN32
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    unsigned char probe;
    return (void *)&probe < tib->StackBase && (void *)&probe >= tib->StackLimit;
#else
    return 1;
#endif
}

/* 1. Run, return to the thread, run again: arguments in both directions. */
static uint64_t seen_init, seen_run, seen_resume;
static int limits_ok = 1;
static ABI void entry_basic(uint64_t init, uint64_t run) {
    seen_init = init; seen_run = run;
    limits_ok &= stack_inside();
    uint64_t resume = 0;
    fiber_return_to_thread(100, &resume);
    seen_resume = resume;
    fiber_return_to_thread(200, &resume);
    for (;;) fiber_return_to_thread(0, &resume);
}
static void test_basic(void) {
    CHECK(fiber_initialize(FIBER(0), "basic", (uint64_t)(uintptr_t)entry_basic, 7, stacks[0], STACK, NULL, 0, 0) == 0);
    uint64_t ret = 0;
    CHECK(fiber_run(FIBER(0), 42, &ret) == 0);
    CHECK(seen_init == 7 && seen_run == 42 && ret == 100);
    CHECK(limits_ok && stack_inside());
    CHECK(fiber_run(FIBER(0), 55, &ret) == 0);
    CHECK(seen_resume == 55 && ret == 200);
    CHECK(fiber_finalize(FIBER(0)) == 0);
}

/* 2. Switch between fibers, GetSelf. */
static Fiber *self_a, *self_b;
static uint64_t b_got, a_got;
static ABI void entry_b(uint64_t init, uint64_t run) {
    (void)init; b_got = run;
    fiber_get_self(&self_b);
    uint64_t back = 0;
    fiber_switch(FIBER(1), 300, &back);
    for (;;) fiber_return_to_thread(0, &back);
}
static ABI void entry_a(uint64_t init, uint64_t run) {
    (void)init; (void)run;
    fiber_get_self(&self_a);
    uint64_t r = 0;
    fiber_switch(FIBER(2), 77, &r);
    a_got = r;
    /* Errors from inside a fiber. */
    uint64_t dummy;
    CHECK(fiber_run(FIBER(2), 0, &dummy) == FIBER_ERROR_PERMISSION);
    CHECK(fiber_switch(FIBER(1), 0, &dummy) == FIBER_ERROR_STATE);
    fiber_return_to_thread(1, &r);
    for (;;) fiber_return_to_thread(0, &r);
}
static void test_switch(void) {
    CHECK(fiber_initialize(FIBER(1), "a", (uint64_t)(uintptr_t)entry_a, 0, stacks[1], STACK, NULL, 0, 0) == 0);
    CHECK(fiber_initialize(FIBER(2), "b", (uint64_t)(uintptr_t)entry_b, 0, stacks[2], STACK, NULL, 0, 0) == 0);
    uint64_t ret = 0;
    CHECK(fiber_run(FIBER(1), 0, &ret) == 0);
    CHECK(ret == 1 && b_got == 77 && a_got == 300);
    CHECK(self_a == FIBER(1) && self_b == FIBER(2));
}

/* 3. A fiber suspended on one thread resumes on another. */
static _Thread_local int thread_tag;
/* Host C code may keep a TLS address across a call; guest code reads fs:[0] each time. The
 * test reads its tag the way the runtime reads its own state (thread_state). */
static __attribute__((noinline)) int read_tag(void) {
    int *p = &thread_tag;
    __asm__ volatile("" : "+r"(p));
    return *p;
}
static int tag_before, tag_after;
static ABI void entry_migrate(uint64_t init, uint64_t run) {
    (void)init; (void)run;
    tag_before = read_tag();
    uint64_t r;
    fiber_return_to_thread(0, &r);
    tag_after = read_tag(); /* after the switch: the other thread's value */
    for (;;) fiber_return_to_thread(0, &r);
}
static void *migrate_thread(void *arg) {
    (void)arg;
    thread_tag = 2;
    uint64_t ret;
    CHECK(fiber_run(FIBER(3), 0, &ret) == 0);
    return NULL;
}
static void test_migration(void) {
    thread_tag = 1;
    CHECK(fiber_initialize(FIBER(3), "migrate", (uint64_t)(uintptr_t)entry_migrate, 0, stacks[3], STACK, NULL, 0, 0) == 0);
    uint64_t ret;
    CHECK(fiber_run(FIBER(3), 0, &ret) == 0);
    pthread_t t;
    pthread_create(&t, NULL, migrate_thread, NULL);
    pthread_join(t, NULL);
    CHECK(tag_before == 1 && tag_after == 2);
}

/* 4. Job-system stress: workers take fibers from a shared queue and run them; each fiber does a
 * little work, sometimes switches to a partner fiber it owns, and returns to whichever thread
 * runs it. Fibers move between threads constantly. */
#define WORKERS 8
#define JOBS 64
#define ROUNDS 2000
static Fiber *queue[JOBS * 2];
static int queue_head, queue_tail;
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t progress[JOBS], partner_runs[JOBS];
static int done_jobs;
static void push(Fiber *f) {
    pthread_mutex_lock(&queue_lock);
    queue[queue_tail++ % (JOBS * 2)] = f;
    pthread_mutex_unlock(&queue_lock);
}
static Fiber *pop(void) {
    pthread_mutex_lock(&queue_lock);
    Fiber *f = queue_head < queue_tail ? queue[queue_head++ % (JOBS * 2)] : NULL;
    pthread_mutex_unlock(&queue_lock);
    return f;
}
static ABI void entry_partner(uint64_t job, uint64_t run) {
    (void)run;
    uint64_t r;
    for (;;) {
        ++partner_runs[job];
        fiber_switch(FIBER(job), job + 1000, &r); /* back to the job fiber that switched here */
    }
}
static ABI void entry_job(uint64_t job, uint64_t run) {
    (void)run;
    uint64_t r;
    for (int i = 0; i < ROUNDS; ++i) {
        volatile uint64_t local = job * 1000003u + (uint64_t)i; /* lives on this fiber's stack */
        if (i % 7 == 0) {
            uint64_t got = 0;
            fiber_switch(FIBER(JOBS + job), 0, &got);
            if (got != job + 1000) { fprintf(stderr, "FAIL: job %llu partner arg %llu\n", (unsigned long long)job, (unsigned long long)got); ++failures; }
        }
        fiber_return_to_thread(0, &r); /* suspend; any worker may resume us */
        if (local != job * 1000003u + (uint64_t)i) { fprintf(stderr, "FAIL: job %llu stack corrupted\n", (unsigned long long)job); ++failures; }
        ++progress[job];
    }
    __atomic_fetch_add(&done_jobs, 1, __ATOMIC_ACQ_REL);
    for (;;) fiber_return_to_thread(1, &r);
}
static void *worker(void *arg) {
    (void)arg;
    while (__atomic_load_n(&done_jobs, __ATOMIC_ACQUIRE) < JOBS) {
        Fiber *f = pop();
        if (!f) { sched_yield(); continue; }
        uint64_t finished = 0;
        int32_t r = fiber_run(f, 0, &finished);
        if (r) { fprintf(stderr, "FAIL: sceFiberRun = 0x%x\n", (unsigned)r); ++failures; return NULL; }
        if (!finished) push(f);
    }
    return NULL;
}
static void test_stress(void) {
    for (int j = 0; j < JOBS; ++j) {
        CHECK(fiber_initialize(FIBER(j), "job", (uint64_t)(uintptr_t)entry_job, (uint64_t)j, stacks[j], STACK, NULL, 0, 0) == 0);
        CHECK(fiber_initialize(FIBER(JOBS + j), "partner", (uint64_t)(uintptr_t)entry_partner, (uint64_t)j,
                               stacks[JOBS + j], STACK / 4, NULL, 0, 0) == 0);
        push(FIBER(j));
    }
    pthread_t threads[WORKERS];
    for (int i = 0; i < WORKERS; ++i) pthread_create(&threads[i], NULL, worker, NULL);
    for (int i = 0; i < WORKERS; ++i) pthread_join(threads[i], NULL);
    uint64_t total = 0;
    for (int j = 0; j < JOBS; ++j) {
        if (progress[j] != ROUNDS) { fprintf(stderr, "FAIL: job %d progress %llu\n", j, (unsigned long long)progress[j]); ++failures; }
        if (partner_runs[j] != (ROUNDS + 6) / 7) { fprintf(stderr, "FAIL: job %d partner runs %llu\n", j, (unsigned long long)partner_runs[j]); ++failures; }
        total += progress[j];
    }
    printf("stress: %d workers, %d jobs, %llu resumptions, %llu switches\n", WORKERS, JOBS,
           (unsigned long long)total, (unsigned long long)switches);
}

/* 5. Errors outside fibers and bad parameters. */
static ABI void entry_none(uint64_t a, uint64_t b) { (void)a; (void)b; }
static void test_errors(void) {
    Fiber *self = NULL;
    CHECK(fiber_get_self(&self) == FIBER_ERROR_PERMISSION);
    uint64_t r;
    CHECK(fiber_return_to_thread(0, &r) == FIBER_ERROR_PERMISSION);
    CHECK(fiber_switch(FIBER(130), 0, &r) == FIBER_ERROR_PERMISSION);
    CHECK(fiber_initialize(NULL, "x", 1, 0, stacks[130], STACK, NULL, 0, 0) == FIBER_ERROR_NULL);
    CHECK(fiber_initialize(FIBER(130), "x", 0, 0, stacks[130], STACK, NULL, 0, 0) == FIBER_ERROR_NULL);
    CHECK(fiber_initialize(FIBER(130), "x", (uint64_t)(uintptr_t)entry_none, 0, stacks[130] + 8, STACK - 16, NULL, 0, 0) == FIBER_ERROR_ALIGNMENT);
    CHECK(fiber_initialize(FIBER(130), "x", (uint64_t)(uintptr_t)entry_none, 0, stacks[130], 256, NULL, 0, 0) == FIBER_ERROR_RANGE);
    CHECK(fiber_initialize((Fiber *)(fibers[130] + 4), "x", (uint64_t)(uintptr_t)entry_none, 0, stacks[130], STACK, NULL, 0, 0) == FIBER_ERROR_ALIGNMENT);
    memset(fibers[131], 0, 256);
    CHECK(fiber_run(FIBER(131), 0, &r) == FIBER_ERROR_INVALID);
    unsigned char opt[128];
    memset(opt, 0xAA, sizeof(opt));
    CHECK(fiber_opt_initialize(opt) == 0 && opt[0] == 0 && opt[127] == 0);
}

#define STEP(f) do { printf("%s...\n", #f); fflush(stdout); f(); } while (0)
int main(void) {
    STEP(test_errors);
    STEP(test_basic);
    STEP(test_switch);
    STEP(test_migration);
    STEP(test_stress);
    runtime_fiber_report();
    if (failures) { printf("fiber-test: %d FAILED\n", failures); return 1; }
    puts("fiber-test: OK");
    return 0;
}
