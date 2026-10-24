#ifndef GOW3_RUNTIME_H
#define GOW3_RUNTIME_H
#include <stdint.h>
#include <stddef.h>
#ifdef _WIN32
#include "compat_win.h"
/* No signals on Windows: the exception handler resumes the thread in gow3_longjmp
 * (gpu/shim/gow3_toggles.h uses the same buffer type). */
typedef gow3_jmp_buf sigjmp_buf;
#define sigsetjmp(buffer, save) gow3_setjmp(buffer)
#else
#include <setjmp.h>
#endif
/* Recovery point for speculative guest memory reads on this thread (probe.c fault handler). */
extern __thread sigjmp_buf *runtime_fault_recover;
/* Restarts the game (in-game settings menu, render resolution change). */
void runtime_restart(void);
#define ABI __attribute__((sysv_abi))
typedef void (ABI *GuestCallback)(void);
void runtime_start(uint64_t capabilities);
uintptr_t runtime_resolve(const char *name, int is_data);
void runtime_report(void);
void runtime_finalize(void *dso);
uintptr_t runtime_mutex_resolve(const char *name);
void runtime_mutex_report(void);
uintptr_t runtime_memory_resolve(const char *name);
void runtime_memory_report(void);
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
int runtime_memory_region_backing(uintptr_t address, uintptr_t *start, uintptr_t *end, int *mapped,
                                  const unsigned char **backing);
const char *runtime_import_name(const char *name);
/* uc3: the game executable's file name (GOW3_EXECUTABLE, default eboot.bin): argv[0], progname. */
const char *runtime_executable_name(void);
/* uc3: guest arguments after the program name (GOW3_GUEST_ARGS); returns how many (<= max). */
int runtime_guest_args(const char **out, int max);
/* uc3: a runtime function by symbol name (sceKernelDlsym), 0 when unknown. */
uintptr_t runtime_resolve_symbol(const char *symbol);
/* uc3: the NID (11 characters) of a symbol name. */
void runtime_nid(const char *symbol, char out[12]);
/* uc3: libSceFiber (runtime_fiber.c). */
uintptr_t runtime_fiber_resolve(const char *name);
void runtime_fiber_report(void);
/* uc3: libSceVideodec2 (runtime_videodec2.c). */
uintptr_t runtime_videodec2_resolve(const char *name);
uintptr_t runtime_rwlock_resolve(const char *name);
void runtime_rwlock_report(void);
void runtime_set_libc_tls(const void *data, uint64_t filesz, uint64_t memsz);
void runtime_set_module_tls(uint64_t module, const void *data, uint64_t filesz, uint64_t memsz);
void runtime_set_procparam(void *param);
void **runtime_application_heap_api(void);
uintptr_t runtime_thread_resolve(const char *name);
void runtime_thread_report(void);
int runtime_thread_snapshot(uint32_t *ids,char (*names)[32],int max);
void runtime_set_main_tls(const void *data, uint64_t filesz, uint64_t memsz, uint64_t align);
void runtime_thread_attach_main(void);
int32_t *runtime_errno(void);
uintptr_t runtime_sema_resolve(const char *name);
void runtime_sema_report(void);
unsigned runtime_sema_waiters(uint32_t id);
uintptr_t runtime_time_resolve(const char *name);
void runtime_content_configure(const uint32_t values[5]);
uintptr_t runtime_content_resolve(const char *name);
void runtime_content_report(void);
typedef struct { const char *name; void *function; } RuntimeExport;
#define RUNTIME_LOOKUP(table, nid) runtime_lookup(table, sizeof(table)/sizeof(*(table)), nid)
const char *runtime_symbol(const char *nid);
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid);
uintptr_t runtime_kernel_resolve(const char *name);
uintptr_t runtime_file_resolve(const char *name);
uintptr_t runtime_services_resolve(const char *name);
void runtime_file_report(void);
void runtime_file_configure(const char *app0, const char *user);
int runtime_file_mount(const char *guest, const char *host);
void runtime_file_unmount(const char *guest);
int runtime_file_translate(const char *guest, char *out, size_t size);
int64_t runtime_file_open(const char *path, int flags, int mode);
int64_t runtime_file_close(int fd);
int64_t runtime_file_read(int fd, void *buffer, uint64_t size);
int64_t runtime_file_pread(int fd, void *buffer, uint64_t size, int64_t offset);
int64_t runtime_file_write(int fd, const void *buffer, uint64_t size);
int64_t runtime_file_lseek(int fd, int64_t offset, int whence);
int64_t runtime_file_stat(const char *path, void *guest_stat);
int64_t runtime_file_fstat(int fd, void *guest_stat);
int64_t runtime_file_getdents(int fd, char *buffer, uint64_t size, int64_t *base);
void runtime_thread_keys_cleanup(void);
/* Guest-visible errno values are FreeBSD's. */
int32_t runtime_guest_errno(int host_errno);
void *runtime_low_map(size_t size, int prot);
/* Windows: reserves the PS4 user address range as a placeholder (no-op elsewhere). */
void runtime_memory_reserve(void);
uintptr_t runtime_ajm_resolve(const char *name);
void runtime_ajm_report(void);
/* AjmDecMp3ParseFrame (guest layout). */
typedef struct {
    uint64_t frame_size;
    uint32_t num_channels, samples_per_channel, bitrate, sample_rate, encoder_delay, num_frames, total_samples, ofl_type;
} Mp3Frame;
typedef struct Mp3Decoder Mp3Decoder;
int runtime_mp3_parse(const unsigned char *data, uint32_t size, int parse_ofl, int layer3_only, Mp3Frame *frame);
Mp3Decoder *runtime_mp3_open(void);
void runtime_mp3_close(Mp3Decoder *decoder);
void runtime_mp3_reset(Mp3Decoder *decoder);
int runtime_mp3_decode(Mp3Decoder *decoder, const unsigned char *data, int size, float *pcm, int max_samples, int *channels);
uintptr_t runtime_videodec_resolve(const char *name);
uintptr_t runtime_audio_resolve(const char *name);
void runtime_audio_report(void);
uintptr_t runtime_pad_resolve(const char *name);
void runtime_pad_report(void);
uintptr_t runtime_rtc_resolve(const char *name);
const char *runtime_file_user_dir(void);
void runtime_savedata_configure(const char *title);
uintptr_t runtime_savedata_resolve(const char *name);
void runtime_savedata_report(void);
/* OrbisSaveDataParam (1328 bytes) of a save directory of the current title. */
#define RUNTIME_SAVE_PARAM_SIZE 1328
int runtime_savedata_param(int32_t user, const char *dir, void *param);
int runtime_savedata_icon(int32_t user, const char *dir, char *out, size_t size);
void runtime_thread_attach_host(const char *name);
/* Host functions the loader runs at the start of guest functions found by their code (probe.c,
 * mouse camera; ported from SuperMedo's uncharted_pc, GPL). `pattern` is hex bytes with ?? for
 * any byte; it must match exactly once in the executable code (and `require` at match+require_at,
 * when given). `moved` bytes of whole position-independent instructions move to the thunk. The
 * callback gets the guest's rdi, rsi and r14 there (instead of r14: rdx when `rdx` is 1, the return
 * address when it is 2, the guest's rbx when it is 3, r12 when it is 4, the thunk's saved registers when it is 5: guest r9
 * at +0x18, the return address at +0x48, stack arguments from +0x50) and the image base. */
typedef struct {
    const char *name, *pattern;
    size_t moved;
    void (__attribute__((ms_abi)) *callback)(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14);
    const char *require;
    int64_t require_at;
    int rdx;
} RuntimeGuestCallback;
const RuntimeGuestCallback *runtime_guest_callbacks(size_t *count);
/* GOW3_MOUSE_U1_CALL98 (probe.c): a "call *0x98(%reg)" at image offset `site` is about to call `target` */
void __attribute__((ms_abi)) runtime_call98_seen(uint64_t rdi, uint64_t rsi, uint64_t site, uint64_t target);
extern volatile uint64_t runtime_image_base;
/* rdx 6: as 5, and the guest's r14 in runtime_guest_r14 (one thread at a time) */
extern volatile uint64_t runtime_guest_r14;
/* The guest's rbp at the last call of a callback whose `rdx` is 2 (diagnostics: its frame chain). */
extern volatile uint64_t runtime_guest_rbp;
/* The return address at the last call of any callback (diagnostics; threads may race). */
extern volatile uint64_t runtime_guest_ret;
#ifdef _WIN32
/* GOW3_MOUSE_AIM_WATCH: write watchpoints (this thread) on two 8-byte fields, counted per writing
 * instruction; on=0 stops counting and the report prints and clears the counts (probe.c). */
void runtime_aim_watch(uint64_t at0, uint64_t at1, int on);
void runtime_aim_watch_report(const char *what);
uint64_t runtime_aim_watch_last(int f); /* the last writer (image offset) of watch address f since the last call, 0 if none */
#endif
/* 1 when input.ini turns mouse look on and GOW3_MOUSE_CAMERA is not 0: the loader installs the
 * mouse camera callbacks. */
int runtime_pad_mouse_camera(void);
/* Controls for the port menu (gow3_overlay.cpp declares these again): the actions with their
 * default keys and controller buttons (pad NULL: keyboard only), and input.ini lines read and
 * changed by key ("key.cross", "mouse_sensitivity"); a change is saved and applies at once. */
typedef struct { const char *name, *label, *key, *pad; } RuntimeInputAction;
const RuntimeInputAction *runtime_input_actions(size_t *count);
int runtime_input_get(const char *key, char *value, size_t size);
int runtime_input_set(const char *key, const char *value);
int runtime_input_available(void);
#endif
