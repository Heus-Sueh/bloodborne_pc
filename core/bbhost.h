/* C interface for the HostPlatform module (gpu/): SDL3 window management,
 * mouse capture, event and pad filtering, and IME text dialogs. */
#ifndef BBHOST_H
#define BBHOST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Keyboard text entry through the game window (IME dialog). begin returns 0 when
 * no window exists; poll returns 0 typing, 1 confirmed, 2 cancelled (UTF-8 text). */
int bbhost_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
int bbhost_text_input_poll(char *out_utf8, uint64_t size);

/* 1 while the in-game settings menu is open: the game's pad input is held neutral. */
int bbhost_overlay_captures_input(void);

/* Mouse look (runtime_pad.c): the window takes the mouse when it is clicked (relative mode) and
 * lets it go on F1, losing focus or the menu opening. take: motion (pixels) and wheel notches
 * since the last call, reset; captured: 1 while the window holds it; enable: mouse_look=0 in
 * bbport.ini keeps the window from taking it. */
void bbhost_mouse_take(double *dx, double *dy, int *wheel_up, int *wheel_down);
int bbhost_mouse_captured(void);

/* Asks the window to take the mouse (relative mode, as a click in the game does); the window
 * thread does it. */
void bbhost_mouse_capture(void);
void bbhost_mouse_look_enable(int enabled);

/* A module's look at the window's events first (an SDL_Event and the SDL_Window, on the window
 * thread): nonzero takes the event (the port and the game do not see it). Up to 4. */
typedef int (*BbHostEventFilter)(const void *sdl_event, void *sdl_window);
void bbhost_add_event_filter(BbHostEventFilter filter);

/* A module's menu pointer: nonzero while the game shows a menu the mouse points into. The window
 * then lets the mouse go and shows the cursor, and takes the mouse back after (as it had it).
 * Up to 4; pointer_wanted: any of them says so. */
typedef int (*BbHostPointerQuery)(void);
void bbhost_add_pointer_query(BbHostPointerQuery query);
int bbhost_pointer_wanted(void);

/* A module's changes to the pad state the game reads (runtime_pad.c, after the host's input and
 * the scripted one): the buttons (PS4 bits); the sticks (lx ly rx ry) as they are. Up to 4. */
typedef void (*BbHostPadFilter)(uint32_t *buttons, const uint8_t *sticks);
void bbhost_add_pad_filter(BbHostPadFilter filter);
void bbhost_filter_pad(uint32_t *buttons, const uint8_t *sticks);

/* The shown frame's width / height (the display buffer's; the window letterboxes it). */
float bbhost_frame_aspect(void);

/* A line of text over the game, near the bottom, until called with NULL (any thread). */
void bbhost_overlay_banner(const char *utf8);

#ifdef __cplusplus
}
#endif

#endif /* BBHOST_H */
