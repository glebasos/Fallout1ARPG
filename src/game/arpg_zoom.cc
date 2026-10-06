// ARPG: mouse-wheel zoom of the map view.
//
// The tile/object renderer draws into a "render buffer" of the view size
// divided by the zoom (bigger when zoomed out, smaller when zoomed in), centered
// on the same tile. Every rect it finishes is scaled into the display window's
// own buffer (bilinear through the palette) and drawn with win_draw_rect, so
// the rest of the window system, the mouse cursor and the HUD never notice.
// Anything that turns the mouse into a map position goes through
// arpg_zoom_screen_to_world; anything that puts a screen element over a map
// position goes through arpg_zoom_world_to_screen.

#include "game/arpg.h"

#include <stdlib.h>
#include <string.h>

#include "game/gconfig.h"
#include "game/map.h"
#include "game/object.h"
#include "game/textobj.h"
#include "game/tile.h"
#include "plib/color/color.h"
#include "plib/gnw/debug.h"
#include "plib/gnw/gnw.h"
#include "plib/gnw/input.h"
#include "plib/gnw/memory.h"
#include "plib/gnw/mouse.h"

namespace fallout {

#define ARPG_CONFIG_SECTION "arpg"

// Zoom steps in percent. 50 is the widest (ARPG_ZOOM_MAX_RENDER_SCALE).
static const int zoom_steps[] = { 50, 60, 70, 80, 90, 100, 115, 130, 150, 175, 200 };
static const int zoom_step_count = sizeof(zoom_steps) / sizeof(zoom_steps[0]);

static unsigned char* view_buf; // display window buffer (screen pixels)
static int view_w;
static int view_h;

static unsigned char* render_buf; // our render buffer, used when zoom != 100
static int render_w;
static int render_h;

static int zoom_pct = 100;
static bool zoom_smooth = true;

// Per destination column/row: the two source samples and the weight (0..256)
// of the second one.
static int* col_src0;
static int* col_src1;
static int* col_frac;
static int* row_src0;
static int* row_src1;
static int* row_frac;

static void zoom_build_axis(int dst, int src, int* s0, int* s1, int* frac)
{
    for (int i = 0; i < dst; i++) {
        // Center of destination pixel i in source pixels, in 1/256ths.
        long long u = ((long long)(2 * i + 1) * src * 256) / (2 * dst) - 128;
        if (u < 0) {
            u = 0;
        }
        int a = (int)(u >> 8);
        int f = (int)(u & 255);
        if (a >= src - 1) {
            a = src - 1;
            f = 0;
        }
        s0[i] = a;
        s1[i] = a + 1 < src ? a + 1 : a;
        frac[i] = f;
    }
}

void arpg_zoom_init(unsigned char* displayBuffer, int width, int height)
{
    view_buf = displayBuffer;
    view_w = width;
    view_h = height;
    render_w = width;
    render_h = height;
    zoom_pct = 100;

    col_src0 = (int*)mem_malloc(sizeof(int) * width * 3);
    col_src1 = col_src0 + width;
    col_frac = col_src1 + width;
    row_src0 = (int*)mem_malloc(sizeof(int) * height * 3);
    row_src1 = row_src0 + height;
    row_frac = row_src1 + height;

    bool smooth = true;
    configGetBool(&game_config, ARPG_CONFIG_SECTION, "zoom_smooth", &smooth);
    zoom_smooth = smooth;

    int pct = 100;
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "zoom", &pct);
    if (arpg_enabled() && pct != 100) {
        arpg_zoom_set_pct(pct);
    }
}

void arpg_zoom_exit()
{
    if (render_buf != NULL) {
        mem_free(render_buf);
        render_buf = NULL;
    }
    if (col_src0 != NULL) {
        mem_free(col_src0);
        col_src0 = NULL;
    }
    if (row_src0 != NULL) {
        mem_free(row_src0);
        row_src0 = NULL;
    }
    view_buf = NULL;
    zoom_pct = 100;
}

int arpg_zoom_pct()
{
    return zoom_pct;
}

bool arpg_zoom_active()
{
    return zoom_pct != 100 && view_buf != NULL;
}

void arpg_zoom_set_pct(int pct)
{
    if (view_buf == NULL) {
        return;
    }

    int minPct = 100 / ARPG_ZOOM_MAX_RENDER_SCALE;
    if (pct < minPct) {
        pct = minPct;
    }
    if (pct > 400) {
        pct = 400;
    }

    if (pct == 100) {
        render_w = view_w;
        render_h = view_h;
        tile_set_buffer(view_buf, view_w, view_h, view_w);
        obj_set_buffer(view_buf, view_w, view_h, view_w);
        text_object_set_buffer(view_buf, view_w, view_h);
    } else {
        if (render_buf == NULL) {
            render_buf = (unsigned char*)mem_malloc(view_w * ARPG_ZOOM_MAX_RENDER_SCALE * view_h * ARPG_ZOOM_MAX_RENDER_SCALE);
            if (render_buf == NULL) {
                return;
            }
        }

        render_w = (view_w * 100 + pct / 2) / pct;
        render_h = (view_h * 100 + pct / 2) / pct;
        memset(render_buf, 0, render_w * render_h);

        zoom_build_axis(view_w, render_w, col_src0, col_src1, col_frac);
        zoom_build_axis(view_h, render_h, row_src0, row_src1, row_frac);

        tile_set_buffer(render_buf, render_w, render_h, render_w);
        obj_set_buffer(render_buf, render_w, render_h, render_w);
        text_object_set_buffer(render_buf, render_w, render_h);
    }

    zoom_pct = pct;
    debug_printf("ARPG: zoom %d%% (render %dx%d)\n", pct, render_w, render_h);
}

void arpg_zoom_step(int delta)
{
    if (view_buf == NULL) {
        return;
    }

    // Nearest step to the current zoom, then move.
    int index = 0;
    for (int i = 0; i < zoom_step_count; i++) {
        if (abs(zoom_steps[i] - zoom_pct) < abs(zoom_steps[index] - zoom_pct)) {
            index = i;
        }
    }
    index += delta;
    if (index < 0) {
        index = 0;
    }
    if (index >= zoom_step_count) {
        index = zoom_step_count - 1;
    }
    if (zoom_steps[index] == zoom_pct) {
        return;
    }

    arpg_zoom_set_pct(zoom_steps[index]);
    tile_refresh_display();
}

void arpg_zoom_view_size(int* width, int* height)
{
    *width = render_w;
    *height = render_h;
}

void arpg_zoom_screen_to_world(int* x, int* y)
{
    if (!arpg_zoom_active()) {
        return;
    }

    // Matches the sampling of zoom_build_axis (pixel centers).
    *x = (int)(((long long)(2 * *x + 1) * render_w) / (2 * view_w));
    *y = (int)(((long long)(2 * *y + 1) * render_h) / (2 * view_h));
}

void arpg_zoom_world_to_screen(int* x, int* y)
{
    if (!arpg_zoom_active()) {
        return;
    }

    *x = (int)(((long long)(2 * *x + 1) * view_w) / (2 * render_w));
    *y = (int)(((long long)(2 * *y + 1) * view_h) / (2 * render_h));
}

void arpg_mouse_world_position(int* x, int* y)
{
    mouse_get_position(x, y);
    arpg_zoom_screen_to_world(x, y);
}

// Palette cycling colours (water, fire, monitors) keep their index so they
// keep animating.
static inline bool zoom_is_cycling(unsigned char c)
{
    return c >= 229 && c <= 254;
}

void arpg_zoom_blit(Rect* rect)
{
    if (!arpg_zoom_active()) {
        return;
    }

    // Destination pixels whose samples can touch the source rect.
    Rect dest;
    dest.ulx = (int)((long long)rect->ulx * view_w / render_w) - 2;
    dest.uly = (int)((long long)rect->uly * view_h / render_h) - 2;
    dest.lrx = (int)((long long)(rect->lrx + 1) * view_w / render_w) + 2;
    dest.lry = (int)((long long)(rect->lry + 1) * view_h / render_h) + 2;
    if (dest.ulx < 0) {
        dest.ulx = 0;
    }
    if (dest.uly < 0) {
        dest.uly = 0;
    }
    if (dest.lrx > view_w - 1) {
        dest.lrx = view_w - 1;
    }
    if (dest.lry > view_h - 1) {
        dest.lry = view_h - 1;
    }
    if (dest.lrx < dest.ulx || dest.lry < dest.uly) {
        return;
    }

    for (int y = dest.uly; y <= dest.lry; y++) {
        unsigned char* out = view_buf + y * view_w;
        const unsigned char* r0 = render_buf + row_src0[y] * render_w;
        const unsigned char* r1 = render_buf + row_src1[y] * render_w;
        int fy = row_frac[y];

        if (!zoom_smooth) {
            const unsigned char* row = fy < 128 ? r0 : r1;
            for (int x = dest.ulx; x <= dest.lrx; x++) {
                out[x] = row[col_frac[x] < 128 ? col_src0[x] : col_src1[x]];
            }
            continue;
        }

        for (int x = dest.ulx; x <= dest.lrx; x++) {
            int fx = col_frac[x];
            unsigned char c00 = r0[col_src0[x]];
            unsigned char c01 = r0[col_src1[x]];
            unsigned char c10 = r1[col_src0[x]];
            unsigned char c11 = r1[col_src1[x]];

            if (c00 == c01 && c00 == c10 && c00 == c11) {
                out[x] = c00;
                continue;
            }

            if (zoom_is_cycling(c00) || zoom_is_cycling(c01) || zoom_is_cycling(c10) || zoom_is_cycling(c11)) {
                out[x] = fy < 128 ? (fx < 128 ? c00 : c01) : (fx < 128 ? c10 : c11);
                continue;
            }

            int w00 = (256 - fx) * (256 - fy);
            int w01 = fx * (256 - fy);
            int w10 = (256 - fx) * fy;
            int w11 = fx * fy;

            // cmap is 6 bits per channel; colorTable takes 5.
            int r = (cmap[c00 * 3] * w00 + cmap[c01 * 3] * w01 + cmap[c10 * 3] * w10 + cmap[c11 * 3] * w11) >> 17;
            int g = (cmap[c00 * 3 + 1] * w00 + cmap[c01 * 3 + 1] * w01 + cmap[c10 * 3 + 1] * w10 + cmap[c11 * 3 + 1] * w11) >> 17;
            int b = (cmap[c00 * 3 + 2] * w00 + cmap[c01 * 3 + 2] * w01 + cmap[c10 * 3 + 2] * w10 + cmap[c11 * 3 + 2] * w11) >> 17;
            out[x] = colorTable[(r << 10) | (g << 5) | b];
        }
    }

    win_draw_rect(display_win, &dest);
}

} // namespace fallout
