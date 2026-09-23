#include "sdl_display.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"

/* LVGL renders RGB565: two bytes per pixel, full screen. */
#define FB_BYTES (UI_HOR_RES * UI_VER_RES * 2)

static SDL_Window   *g_window;
static SDL_Renderer *g_renderer;
static SDL_Texture  *g_texture;
static uint8_t       g_fb[FB_BYTES];
static uint32_t      g_pixels[UI_HOR_RES * UI_VER_RES];

/*
 * SIM_SHOW_GREY=1 skips the one-bit quantisation and shows LVGL's anti-aliased
 * render instead.
 *
 * This is a diagnostic, not a preview. The ST7305 is genuinely one bit per
 * pixel - its datasheet has no grayscale mode at all, and 0x3A (DTFORM) selects
 * SPI data packing, not colour depth - so the glass can never look like this.
 * Use it to see what the renderer produced before thresholding; judge the
 * design on the default view.
 */
static bool g_show_grey;

/*
 * Simulator colours.
 *
 * Pure black on white by default - it reads better on a monitor and in
 * screenshots. Set SIM_REFLECTIVE_TINT to 1 for the warm grey-green ground the
 * real panel actually has; the layout is identical either way, but the tinted
 * version is the honest preview of contrast on glass. Worth a look before
 * committing to anything that depends on fine contrast.
 */
#define SIM_REFLECTIVE_TINT 0

#if SIM_REFLECTIVE_TINT
#define INK   0xFF1A1A18u
#define PAPER 0xFFD8D8CEu
#else
#define INK   0xFF000000u
#define PAPER 0xFFFFFFFFu
#endif

/*
 * Threshold LVGL's anti-aliased RGB565 to one bit with ui_px_is_paper() - the
 * exact rule board.c applies on the device - then paint it in the panel's two
 * colours. The window is therefore a true preview: the simulator sees the same
 * quantisation the glass will, not a softer anti-aliased version of it.
 */
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    const int32_t   w   = area->x2 - area->x1 + 1;
    const uint16_t *src = (const uint16_t *)(const void *)px_map;

    for (int32_t y = area->y1; y <= area->y2; y++) {
        const int32_t row = y - area->y1;
        for (int32_t x = area->x1; x <= area->x2; x++) {
            const uint16_t px = src[row * w + (x - area->x1)];
            uint32_t out;

            if (g_show_grey) {
                /* LVGL's anti-aliased output, before the panel's one-bit
                 * quantisation. Useful only for seeing what the renderer
                 * produced - the glass cannot show this. */
                const uint32_t r = (uint32_t)((px >> 11) & 0x1F) << 3;
                const uint32_t g = (uint32_t)((px >> 5) & 0x3F) << 2;
                const uint32_t b = (uint32_t)(px & 0x1F) << 3;
                const uint32_t l = (r * 77u + g * 151u + b * 28u) >> 8;
                out = 0xFF000000u | (l << 16) | (l << 8) | l;
            } else {
                out = ui_px_is_paper(px) ? PAPER : INK;
            }
            g_pixels[y * UI_HOR_RES + x] = out;
        }
    }

    if (lv_display_flush_is_last(disp)) {
        SDL_UpdateTexture(g_texture, NULL, g_pixels,
                          UI_HOR_RES * (int)sizeof(uint32_t));
        SDL_RenderClear(g_renderer);
        SDL_RenderCopy(g_renderer, g_texture, NULL, NULL);
        SDL_RenderPresent(g_renderer);
    }
    lv_display_flush_ready(disp);
}

lv_display_t *sim_display_init(void)
{
    g_show_grey = getenv("SIM_SHOW_GREY") != NULL;
    if (g_show_grey) {
        fprintf(stderr, "SIM_SHOW_GREY: showing LVGL's anti-aliased output; "
                        "the panel cannot render this\n");
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return NULL;
    }

    g_window = SDL_CreateWindow("Daly BMS monitor - ESP32-S3-RLCD-4.2",
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                UI_VISIBLE_W * SIM_ZOOM, UI_VER_RES * SIM_ZOOM,
                                SDL_WINDOW_SHOWN);
    if (!g_window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return NULL;
    }
    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
    g_texture  = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888,
                                   SDL_TEXTUREACCESS_STREAMING,
                                   UI_HOR_RES, UI_VER_RES);

    for (int i = 0; i < UI_HOR_RES * UI_VER_RES; i++) {
        g_pixels[i] = PAPER;
    }

    lv_display_t *disp = lv_display_create(UI_HOR_RES, UI_VER_RES);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, g_fb, NULL, sizeof g_fb,
                           LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, flush_cb);
    return disp;
}

bool sim_poll_events(void)
{
    SDL_Event e;

    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) {
            return false;
        }
        if (e.type == SDL_KEYDOWN) {
            switch (e.key.keysym.sym) {
            case SDLK_ESCAPE:
            case SDLK_q:
                return false;
            /* The board has one usable button. `n` is a press, `N` a long press. */
            case SDLK_n:
                ui_input(e.key.keysym.mod & KMOD_SHIFT ? UI_KEY_LONG
                                                       : UI_KEY_SHORT);
                break;
            default:
                break;
            }
        }
    }
    return true;
}

bool sim_screenshot(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "screenshot: cannot write %s\n", path);
        return false;
    }
    /* Binary PPM of the panel exactly as it will look. */
    fprintf(f, "P6\n%d %d\n255\n", UI_VISIBLE_W, UI_VER_RES);
    for (int y = 0; y < UI_VER_RES; y++) {
        for (int x = 0; x < UI_VISIBLE_W; x++) {
            const uint32_t px = g_pixels[y * UI_HOR_RES + x];
            const uint8_t rgb[3] = { (uint8_t)(px >> 16), (uint8_t)(px >> 8),
                                     (uint8_t)px };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    return true;
}

void sim_deinit(void)
{
    if (g_texture)  SDL_DestroyTexture(g_texture);
    if (g_renderer) SDL_DestroyRenderer(g_renderer);
    if (g_window)   SDL_DestroyWindow(g_window);
    SDL_Quit();
}
