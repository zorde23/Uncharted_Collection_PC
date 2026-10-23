/* C interface of the GPU library (gpu/): shadPS4's Liverpool/Vulkan video core,
 * GnmDriver, VideoOut and kernel event queues, adapted to the native loader. */
#ifndef GOW3GPU_H
#define GOW3GPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *title;          /* window title */
    const char *serial;         /* CUSA id, names the pipeline cache */
    const char *user_dir;       /* pipeline cache/logs directory */
    uint32_t sdk_version;       /* from the eboot's procparam */
    uint32_t psf_attributes;    /* param.sfo ATTRIBUTE */
    int32_t width, height;      /* initial window size */
} Gow3GpuConfig;
/* Registers kernel event queues (needed with or without graphics). */
void gow3gpu_register_kernel(void);
/* Creates window, Vulkan device, presenter and GPU command processor. */
int gow3gpu_init(const Gow3GpuConfig *config);
/* Function for an imported NID ("NID#lib#mod"), or 0 when the GPU library does not provide it. */
uintptr_t gow3gpu_resolve(const char *scoped_nid);
/* Called first by the loader's SIGSEGV handler: 1 when a GPU page-tracking fault was handled. */
int gow3gpu_handle_fault(void *ucontext, void *address);
/* GOW3_WRITE_LOG=1: prints the logged GPU-side writes to guest memory near the fault. */
void gow3gpu_dump_guest_writes(void *ucontext);
/* Keyboard text entry through the game window (IME dialog). begin returns 0 when
 * no window exists; poll returns 0 typing, 1 confirmed, 2 cancelled (UTF-8 text). */
int gow3gpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
int gow3gpu_text_input_poll(char *out_utf8, uint64_t size);
/* A list for the player to choose from, drawn over the game (save list). images: a PNG path per
 * item drawn beside it (NULL array or entry: none). begin: 0 when no window exists.
 * poll: -1 while open, -2 cancelled, else the chosen index. */
int gow3gpu_choice_begin(const char *title, const char *const *items, const char *const *images, int count, int focus);
int gow3gpu_choice_poll(void);
/* 1 while the in-game settings menu is open: the game's pad input is held neutral. */
int gow3gpu_overlay_captures_input(void);
/* Keyboard and mouse controls (runtime_pad.c). configure: 1 holds the cursor in the window
 * (relative mode) for mouse look while the game has the focus. sample takes the motion (pixels)
 * and wheel steps since the last call and the held buttons (bit 0 left, 1 right, 2 middle,
 * 3-4 side); returns 1 while the cursor is held. */
void gow3gpu_mouse_configure(int look);
int gow3gpu_mouse_sample(float *dx, float *dy, uint32_t *buttons, int *wheel);
/* Number of symbols registered by the vendored libraries (diagnostics). */
unsigned gow3gpu_symbol_count(void);
/* Windows thread id of the GPU command processor thread (0 until it starts; diagnostics). */
unsigned long gow3gpu_cp_thread_id(void);
/* uc3 GOW3_CB_WATCH: the guest's frame (submitDone calls) and the end-of-pipe flips reached. */
void gow3gpu_frame_counters(uint64_t *submit_done, uint64_t *eop_flips);
#ifdef __cplusplus
}
#endif
#endif
