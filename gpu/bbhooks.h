/* C interface for the GameHooks module (gpu/): reverse-engineered guest game
 * mechanics (camera look, free camera, projection parameters, code patching). */
#ifndef BBHOOKS_H
#define BBHOOKS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Native mouse look: turns the game's camera (radians, applied at its next update); 0 when the
 * camera hook is not in (another game version, BB_NATIVE_MOUSE=0) or the camera does not take the
 * turns now (lock-on, menus): drive the right stick then. */
int bbhook_camera_look(float yaw, float pitch);

/* 1 while the free camera (the community patch) is on: the mouse drives it through the right stick. */
int bbhook_free_camera(void);

/* The game's current camera state (its projection parameters at +0x50), 0 until known. */
uint64_t bbhook_camera_state(void);

/* Patches the loaded image before the game runs (image still writable): libGnm entry hooks. */
void bbhook_patch_image(unsigned char *image, uint64_t size);

#ifdef __cplusplus
}
#endif

#endif /* BBHOOKS_H */
