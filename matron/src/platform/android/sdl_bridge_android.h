/*
 * sdl_bridge_android.h
 *
 * on android SDL belongs to the activity's SDL thread (SDL_main), but matron draws from its
 * own thread. the SDL thread runs sdl_bridge_run(): it owns the window, draws the device
 * (platform/macos/device_skin.c) with matron's frames on its oled, and turns touches on the
 * keys and knobs into key/enc events. the screen (hardware/screen/sdl.cc) calls the rest.
 */

#pragma once

#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

// SDL thread only
extern int sdl_bridge_init(void); // 0 = ok
extern void sdl_bridge_run(void); // returns when the app quits

// any thread
extern SDL_Window *sdl_bridge_create_window(const char *title, int w, int h);
extern void sdl_bridge_present(SDL_Window *window, SDL_Surface *src);
extern void sdl_bridge_destroy_window(SDL_Window *window);

#ifdef __cplusplus
}
#endif
