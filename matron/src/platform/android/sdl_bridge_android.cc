/*
 * sdl_bridge_android.cc
 *
 * the norns window on android (see sdl_bridge_android.h). the device is drawn with the macOS
 * port's skin (platform/macos/device_skin.c, pure cairo): K1-K3 and E1-E3 where they sit on
 * a norns, the oled behind its bezel, the button row along the top.
 *
 * touch: every finger works on its own, so K1 can be held while another finger turns E3.
 *  - a key: pressed while the finger is down
 *  - a knob: drag from it; right or up turns clockwise
 *  - MIX TAPE HOME PARAMS, MANUAL, MIC: act on release over the same button
 * a hardware keyboard drives the keys and knobs as on the mac (1 2 3, QW AS ZX) through
 * lua/core/keyboard.lua when NORNS_HARDWARE_KEYS is set; this file only handles touch.
 */

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#include <cairo.h>

#include "event_types.h"
#include "events.h"
#include "platform/android/sdl_bridge_android.h"
#include "platform/macos/device_skin.h"

namespace {

SDL_Window *window = NULL;
SDL_Renderer *renderer = NULL;
SDL_Texture *texture = NULL;
int tex_w = 0, tex_h = 0;

// matron's frame, converted on its thread (present), drawn on the SDL thread
pthread_mutex_t frame_lock = PTHREAD_MUTEX_INITIALIZER;
cairo_surface_t *oled = NULL; // 128x64 RGB24
bool have_frame = false;
bool closed = false;

Uint32 wake_event = 0; // SDL user event: a new frame or a control changed

// drawing state: SDL thread only
cairo_surface_t *canvas = NULL;    // ARGB32, window size
cairo_surface_t *static_bg = NULL; // skin_draw_static, window size
skin_geom_t geom;
skin_state_t skin;
bool foreground = true;

// standard norns, in layout units (as platform/macos/device_skin.c)
const double key_pos[3][2] = {{109, 252}, {594, 542}, {749, 542}};
const double enc_pos[3][2] = {{216, 252}, {672, 420}, {827, 420}};
// a finger is bigger than a mouse pointer: anything this close to a key or knob centre
// (layout units) is that key or knob
const double touch_r = 70;
// finger travel per detent, layout units
const double drag_step = 6.0;

#define MAX_FINGERS 10
struct finger {
    SDL_FingerID id;
    bool used;
    int key;    // 1-3 held, 0 none
    int enc;    // 1-3 turned, 0 none
    int button; // SKIN_PAGE1.. pressed, 0 none
    double x, y, acc;
};
finger fingers[MAX_FINGERS];

void post_key(int n, int z) {
    union event_data *ev = event_data_new(EVENT_KEY);
    ev->key.n = n;
    ev->key.val = z;
    event_post(ev);
}

void post_enc(int n, int delta) {
    union event_data *ev = event_data_new(EVENT_ENC);
    ev->enc.n = n;
    ev->enc.delta = delta;
    event_post(ev);
}

// a button on the device for lua (_norns.window_button): 1-4 menu page, 5 manual, 6 mic
void post_window_button(int n) {
    union event_data *ev = event_data_new(EVENT_WINDOW_BUTTON);
    ev->key.n = n;
    ev->key.val = 1;
    event_post(ev);
}

void wake(void) {
    if (wake_event != 0 && wake_event != (Uint32)-1) {
        SDL_Event e;
        SDL_zero(e);
        e.type = wake_event;
        SDL_PushEvent(&e);
    }
}

double units_x(double px) {
    return (px - geom.ox) / geom.s;
}
double units_y(double py) {
    return (py - geom.oy) / geom.s;
}

// control under the finger: keys and knobs by distance, buttons by the skin
int touch_hit(double px, double py) {
    double ux = units_x(px), uy = units_y(py);
    int best = SKIN_NONE;
    double best_d = touch_r;
    for (int i = 0; i < 3; i++) {
        double d = hypot(ux - key_pos[i][0], uy - key_pos[i][1]);
        if (d < best_d) {
            best_d = d;
            best = SKIN_K1 + i;
        }
        d = hypot(ux - enc_pos[i][0], uy - enc_pos[i][1]);
        if (d < best_d) {
            best_d = d;
            best = SKIN_E1 + i;
        }
    }
    if (best != SKIN_NONE) {
        return best;
    }
    return skin_hit(&geom, px, py);
}

void free_surfaces(void) {
    if (canvas != NULL) {
        cairo_surface_destroy(canvas);
        canvas = NULL;
    }
    if (static_bg != NULL) {
        cairo_surface_destroy(static_bg);
        static_bg = NULL;
    }
    if (texture != NULL) {
        SDL_DestroyTexture(texture);
        texture = NULL;
    }
}

bool layout(void) {
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(renderer, &w, &h);
    if (w <= 0 || h <= 0) {
        return false;
    }
    if (canvas != NULL && texture != NULL && w == tex_w && h == tex_h) {
        return true;
    }
    free_surfaces();
    tex_w = w;
    tex_h = h;
    skin_geom(w, h, &geom);
    canvas = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    static_bg = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(static_bg);
    skin_draw_static(cr, &geom);
    cairo_destroy(cr);
    // cairo ARGB32 is native-endian ARGB: SDL's ARGB8888
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    return texture != NULL;
}

void draw(void) {
    if (!foreground || renderer == NULL || !layout()) {
        return;
    }
    cairo_t *cr = cairo_create(canvas);
    cairo_set_source_surface(cr, static_bg, 0, 0);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    skin_draw_controls(cr, &geom, &skin);
    pthread_mutex_lock(&frame_lock);
    if (have_frame && oled != NULL) {
        skin_draw_oled(cr, &geom, oled);
    }
    pthread_mutex_unlock(&frame_lock);
    cairo_destroy(cr);
    cairo_surface_flush(canvas);
    SDL_UpdateTexture(texture, NULL, cairo_image_surface_get_data(canvas), cairo_image_surface_get_stride(canvas));
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
}

finger *finger_for(SDL_FingerID id, bool create) {
    for (int i = 0; i < MAX_FINGERS; i++) {
        if (fingers[i].used && fingers[i].id == id) {
            return &fingers[i];
        }
    }
    if (!create) {
        return NULL;
    }
    for (int i = 0; i < MAX_FINGERS; i++) {
        if (!fingers[i].used) {
            memset(&fingers[i], 0, sizeof(finger));
            fingers[i].used = true;
            fingers[i].id = id;
            return &fingers[i];
        }
    }
    return NULL;
}

void button_action(int c) {
    if (c >= SKIN_PAGE1 && c <= SKIN_PAGE4) {
        skin.page = c - SKIN_PAGE1 + 1;
        post_window_button(c - SKIN_PAGE1 + 1);
    } else if (c == SKIN_MANUAL) {
        post_window_button(5);
    } else if (c == SKIN_MIC) {
        post_window_button(6);
    }
    // GRID ARC KEYS MAIDEN, the OUT fader and MUTE: not on android yet
}

void finger_down(const SDL_TouchFingerEvent *e) {
    double px = e->x * tex_w, py = e->y * tex_h;
    finger *f = finger_for(e->fingerId, true);
    if (f == NULL) {
        return;
    }
    f->x = px;
    f->y = py;
    int c = touch_hit(px, py);
    if (c >= SKIN_K1 && c <= SKIN_K3) {
        f->key = c - SKIN_K1 + 1;
        skin.key_down[f->key - 1] = 1;
        post_key(f->key, 1);
    } else if (c >= SKIN_E1 && c <= SKIN_E3) {
        f->enc = c - SKIN_E1 + 1;
        f->acc = 0;
    } else if (c >= SKIN_PAGE1 && c <= SKIN_MANUAL) {
        f->button = c;
        skin.button_down = c;
    }
    draw();
}

void finger_motion(const SDL_TouchFingerEvent *e) {
    finger *f = finger_for(e->fingerId, false);
    if (f == NULL) {
        return;
    }
    double px = e->x * tex_w, py = e->y * tex_h;
    if (f->enc != 0) {
        f->acc += ((px - f->x) - (py - f->y)) / (geom.s > 0 ? geom.s : 1);
        int ticks = (int)(f->acc / drag_step);
        if (ticks != 0) {
            f->acc -= ticks * drag_step;
            skin.enc_ticks[f->enc - 1] += ticks;
            post_enc(f->enc, ticks);
            draw();
        }
    }
    f->x = px;
    f->y = py;
}

void finger_up(const SDL_TouchFingerEvent *e) {
    finger *f = finger_for(e->fingerId, false);
    if (f == NULL) {
        return;
    }
    if (f->key != 0) {
        skin.key_down[f->key - 1] = 0;
        post_key(f->key, 0);
    }
    if (f->button != 0) {
        if (skin.button_down == f->button) {
            skin.button_down = SKIN_NONE;
        }
        if (e->type == SDL_FINGERUP && skin_hit(&geom, e->x * tex_w, e->y * tex_h) == f->button) {
            button_action(f->button);
        }
    }
    f->used = false;
    draw();
}

// the app goes to the background: let go of everything held
void release_all(void) {
    for (int i = 0; i < MAX_FINGERS; i++) {
        if (fingers[i].used && fingers[i].key != 0) {
            skin.key_down[fingers[i].key - 1] = 0;
            post_key(fingers[i].key, 0);
        }
        fingers[i].used = false;
    }
    skin.button_down = SKIN_NONE;
}

} // namespace

int sdl_bridge_init(void) {
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "sdl_bridge: SDL_Init: %s\n", SDL_GetError());
        return -1;
    }
    window = SDL_CreateWindow("Verdandi", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 0, 0,
                              SDL_WINDOW_FULLSCREEN | SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
    if (window == NULL) {
        fprintf(stderr, "sdl_bridge: SDL_CreateWindow: %s\n", SDL_GetError());
        return -1;
    }
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (renderer == NULL) {
        renderer = SDL_CreateRenderer(window, -1, 0);
    }
    if (renderer == NULL) {
        fprintf(stderr, "sdl_bridge: SDL_CreateRenderer: %s\n", SDL_GetError());
        return -1;
    }
    wake_event = SDL_RegisterEvents(1);
    memset(&skin, 0, sizeof(skin));
    skin.input = SKIN_INPUT_OFF;
    skin.out_level = 0.75;
    oled = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 128, 64);
    draw();
    return 0;
}

void sdl_bridge_run(void) {
    SDL_Event e;
    bool quit = false;
    while (!quit) {
        if (!SDL_WaitEventTimeout(&e, 500)) {
            continue;
        }
        bool redraw = false;
        do {
            switch (e.type) {
            case SDL_QUIT:
                quit = true;
                break;
            case SDL_APP_WILLENTERBACKGROUND:
                foreground = false;
                release_all();
                break;
            case SDL_APP_DIDENTERFOREGROUND:
                foreground = true;
                redraw = true;
                break;
            case SDL_RENDER_DEVICE_RESET:
            case SDL_RENDER_TARGETS_RESET:
                free_surfaces();
                redraw = true;
                break;
            case SDL_WINDOWEVENT:
                redraw = true;
                break;
            case SDL_FINGERDOWN:
                finger_down(&e.tfinger);
                break;
            case SDL_FINGERMOTION:
                finger_motion(&e.tfinger);
                break;
            case SDL_FINGERUP:
                finger_up(&e.tfinger);
                break;
            default:
                if (e.type == wake_event) {
                    redraw = true;
                }
                break;
            }
        } while (!quit && SDL_PollEvent(&e));
        if (redraw) {
            draw();
        }
    }
    pthread_mutex_lock(&frame_lock);
    closed = true;
    pthread_mutex_unlock(&frame_lock);
}

SDL_Window *sdl_bridge_create_window(const char *title, int w, int h) {
    (void)title;
    (void)w;
    (void)h;
    return window;
}

void sdl_bridge_present(SDL_Window *win, SDL_Surface *src) {
    if (win == NULL || src == NULL) {
        return;
    }
    pthread_mutex_lock(&frame_lock);
    if (!closed && oled != NULL && src->w == 128 && src->h == 64) {
        cairo_surface_flush(oled);
        SDL_ConvertPixels(src->w, src->h, src->format->format, src->pixels, src->pitch, SDL_PIXELFORMAT_RGB888,
                          cairo_image_surface_get_data(oled), cairo_image_surface_get_stride(oled));
        cairo_surface_mark_dirty(oled);
        have_frame = true;
    }
    pthread_mutex_unlock(&frame_lock);
    wake();
}

void sdl_bridge_destroy_window(SDL_Window *win) {
    (void)win;
}
