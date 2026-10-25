/* C interface of the GPU library (gpu/): the command processor and Vulkan renderer,
 * GnmDriver, VideoOut and kernel event queues, adapted to the native loader. */
#ifndef BBGPU_H
#define BBGPU_H

#include <stdint.h>
#include "bbhost.h"
#include "bbhooks.h"

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
} BbGpuConfig;

/* Registers kernel event queues (needed with or without graphics). */
void bbgpu_register_kernel(void);

/* Creates window, Vulkan device, presenter and GPU command processor. */
int bbgpu_init(const BbGpuConfig *config);

/* Function for an imported NID ("NID#lib#mod"), or 0 when the GPU library does not provide it. */
uintptr_t bbgpu_resolve(const char *scoped_nid);

/* Called first by the loader's SIGSEGV handler: 1 when a GPU page-tracking fault was handled. */
int bbgpu_handle_fault(void *ucontext, void *address);

/* BB_WRITE_LOG=1: prints the logged GPU-side writes to guest memory near the fault. */
void bbgpu_dump_guest_writes(void *ucontext);

/* Number of symbols registered by the vendored libraries (diagnostics). */
unsigned bbgpu_symbol_count(void);

/* Backward compatibility transitional forwarders and aliases for HostPlatform and GameHooks */
typedef BbHostEventFilter BbGpuEventFilter;
typedef BbHostPointerQuery BbGpuPointerQuery;
typedef BbHostPadFilter BbGpuPadFilter;

void bbgpu_add_event_filter(BbHostEventFilter filter);
void bbgpu_add_pointer_query(BbHostPointerQuery query);
int bbgpu_pointer_wanted(void);
void bbgpu_add_pad_filter(BbHostPadFilter filter);
void bbgpu_filter_pad(uint32_t *buttons, const uint8_t *sticks);
float bbgpu_frame_aspect(void);
void bbgpu_overlay_banner(const char *utf8);
int bbgpu_overlay_captures_input(void);
void bbgpu_mouse_take(double *dx, double *dy, int *wheel_up, int *wheel_down);
int bbgpu_mouse_captured(void);
void bbgpu_mouse_capture(void);
void bbgpu_mouse_look_enable(int enabled);
int bbgpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
int bbgpu_text_input_poll(char *out_utf8, uint64_t size);

int bbgpu_camera_look(float yaw, float pitch);
int bbgpu_free_camera(void);
uint64_t bbgpu_camera_state(void);
void bbgpu_patch_image(unsigned char *image, uint64_t size);

#ifdef __cplusplus
}
#endif

#endif /* BBGPU_H */
